#!/usr/bin/env python3
"""Kernel micro-bench: ns per call for the power-function kernels, on this host.

Builds and runs the ``mm_bench`` target (``test/bench/bench_kernels.cpp``) in the desktop build
tree and prints its Markdown table. A report, not a check: nothing passes or fails, and the
numbers are read against the previous rows in performance.md before a kernel swap is accepted
(the generative-fields plan's 1.3x-per-sample bound on the gradient-noise swap is the first).

Host timings only. The S3 is 20-40x slower per core (performance.md), so the RATIO between two
rows is what transfers to a board; run collect_kpi.py on the device for the absolute cost.

Usage:
    uv run moondeck/check/bench_kernels.py            # build (Release) and run
    uv run moondeck/check/bench_kernels.py --no-build # run the binary already built
"""

import argparse
import os
import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = ROOT / "build" / "macos"


def binary() -> Path:
    exe = "mm_bench.exe" if platform.system() == "Windows" else "mm_bench"
    for candidate in (BUILD_DIR / "test" / exe, BUILD_DIR / "test" / "Release" / exe):
        if candidate.exists():
            return candidate
    return BUILD_DIR / "test" / exe


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-build", action="store_true", help="run the already-built binary")
    args = ap.parse_args()

    if not args.no_build:
        # Same tree and build type build_desktop.py uses for the test binaries, one target only.
        cfg = ["cmake", "-B", str(BUILD_DIR), "-DCMAKE_BUILD_TYPE=Release"]
        if subprocess.run(cfg, cwd=ROOT).returncode != 0:
            sys.exit(1)
        cmd = ["cmake", "--build", str(BUILD_DIR), "--target", "mm_bench"]
        if platform.system() == "Windows":
            cmd += ["--config", "Release"]
        if subprocess.run(cmd, cwd=ROOT).returncode != 0:
            sys.exit(1)

    exe = binary()
    if not exe.exists():
        print(f"mm_bench not built: {exe}\n  build: uv run moondeck/check/bench_kernels.py")
        sys.exit(1)
    print(f"Kernel micro-bench on {platform.machine()} ({os.cpu_count()} cores), Release, best of 5:", flush=True)
    sys.exit(subprocess.run([str(exe)], cwd=ROOT).returncode)


if __name__ == "__main__":
    main()
