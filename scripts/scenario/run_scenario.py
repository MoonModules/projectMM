#!/usr/bin/env python3
"""Run scenario tests. Replays scenario JSON files via the in-process runner."""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SCENARIOS_DIR = ROOT / "test" / "scenarios"

_HOST = {"darwin": "macos", "win32": "windows"}.get(sys.platform, "linux")
_RUNNER_BASE = ROOT / "build" / _HOST / "test" / "mm_scenarios"
RUNNER = _RUNNER_BASE.with_suffix(".exe") if sys.platform == "win32" else _RUNNER_BASE

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", default=None,
                        help="Scenario name (without .json). Runs all if omitted.")
    args = parser.parse_args()

    if not RUNNER.exists():
        print(f"Scenario runner not found: {RUNNER}")
        print("Run build_desktop.py first.")
        sys.exit(1)

    if args.name:
        scenario_file = SCENARIOS_DIR / f"{args.name}.json"
        if not scenario_file.exists():
            print(f"Scenario not found: {scenario_file}")
            sys.exit(1)
        r = subprocess.run([str(RUNNER), str(scenario_file)], cwd=ROOT)
    else:
        # Run all scenarios (runner auto-discovers when no arg given)
        r = subprocess.run([str(RUNNER)], cwd=ROOT)

    sys.exit(r.returncode)

if __name__ == "__main__":
    main()
