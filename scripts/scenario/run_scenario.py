#!/usr/bin/env python3
"""Run scenario tests. Replays scenario JSON files via the in-process runner.

Filters compose:
  --name <stem>     run that one scenario file (the JSON stem, e.g. scenario_Layer_base_pipeline)
  --module <Name>   run every scenario whose top-level `module` (or `also`) matches
  --name + --module the named scenario must also match the module (otherwise refused)
  (neither)         run every scenario the runner discovers
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
# Reuse the shared test-metadata parser so scenario discovery stays in one place.
sys.path.insert(0, str(ROOT / "scripts" / "docs"))
import _test_metadata as test_meta  # noqa: E402

_HOST = {"darwin": "macos", "win32": "windows"}.get(sys.platform, "linux")
_RUNNER_BASE = ROOT / "build" / _HOST / "test" / "mm_scenarios"
RUNNER = _RUNNER_BASE.with_suffix(".exe") if sys.platform == "win32" else _RUNNER_BASE


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--name", default=None,
                        help="Scenario name (file stem). Runs all if omitted.")
    parser.add_argument("--module", default=None,
                        help="Module filter. Runs only scenarios that match.")
    args = parser.parse_args()

    if not RUNNER.exists():
        print(f"Scenario runner not found: {RUNNER}")
        print("Run build_desktop.py first.")
        sys.exit(1)

    module_filter = args.module if (args.module and args.module.lower() != "all") else None

    # Resolve the scenario set.
    if args.name:
        scenario_file = test_meta.find_scenario_path(args.name)
        if not scenario_file:
            print(f"Scenario not found: {args.name}.json under {test_meta.SCENARIO_DIR}")
            sys.exit(1)
        if module_filter and scenario_file not in test_meta.paths_for_module(module_filter):
            print(f"Scenario {args.name} does not match module {module_filter}.")
            sys.exit(1)
        r = subprocess.run([str(RUNNER), str(scenario_file)], cwd=ROOT)
        sys.exit(r.returncode)

    if module_filter:
        paths = test_meta.paths_for_module(module_filter)
        if not paths:
            print(f"No scenarios found for module: {module_filter}")
            sys.exit(1)
        print(f"Module filter: {module_filter} ({len(paths)} scenario(s))")
        failed = 0
        for p in paths:
            r = subprocess.run([str(RUNNER), str(p)], cwd=ROOT)
            if r.returncode != 0:
                failed += 1
        sys.exit(1 if failed else 0)

    # Run all scenarios (runner auto-discovers when no arg given)
    r = subprocess.run([str(RUNNER)], cwd=ROOT)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
