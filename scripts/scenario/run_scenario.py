#!/usr/bin/env python3
"""Run scenario tests. Replays scenario JSON files via the in-process runner.

Filters compose:
  --name <stem>     run that one scenario file (the JSON stem, e.g. scenario_Layer_base_pipeline)
  --module <Name>   run every scenario whose top-level `module` (or `also`) matches
  --name + --module the named scenario must also match the module (otherwise refused)
  (neither)         run every scenario the runner discovers

--update-contract   renegotiate the per-step performance contract. After each
--reason "..."      scenario, parse its MEASURE lines and write the observed
                    tick_us / free_heap into contract[<host-target>] along with
                    set_by + reason. This is the "I want to change the promise"
                    path, not a routine baseline refresh. Both flags required
                    together; symmetric with run_live_scenario.py.
"""

import argparse
import datetime
import json
import platform
import re
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

# Format emitted by scenario_runner.cpp's measure block:
#   MEASURE <step-name>: tick=Nus FPS=N lights=N heap=±N (step: ±N) block=N
# `<step-name>` may contain hyphens and underscores. heap is signed offset
# (vs heapBefore); block is the absolute largest contiguous block.
# Both are 0 on desktop where the platform stubs return "unlimited".
_MEASURE_RE = re.compile(
    r"^\s*MEASURE\s+(?P<name>\S+):\s+tick=(?P<tick>\d+)us\s+FPS=\d+\s+lights=\d+"
    r"\s+heap=(?P<heap>[+-]?\d+).*?\bblock=(?P<block>\d+)"
)


def _host_target() -> str:
    """Same shape run_live_scenario.py's _detect_target falls back to on desktop."""
    return {"Darwin": "pc-macos", "Linux": "pc-linux", "Windows": "pc-windows"}.get(
        platform.system(), "pc-unknown"
    )


def _run_one(path: Path, update_contract: bool, update_reason: str | None) -> int:
    """Run one scenario. Always parses MEASURE lines and writes
    observed.<target> blocks back into the scenario JSON (every run produces a
    drift record). With --update-contract, also rewrites the contract.

    Symmetric with the live runner's behaviour — observations persist always,
    contracts only when renegotiated."""
    # Capture + tee: stream to stdout while collecting MEASURE lines.
    proc = subprocess.Popen([str(RUNNER), str(path)], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1)
    observations: dict[str, dict] = {}  # step-name → {tick_us, free_heap, max_alloc_block}
    for line in proc.stdout:
        sys.stdout.write(line)
        m = _MEASURE_RE.match(line)
        if m:
            heap = int(m["heap"])
            # heap is reported as a signed offset relative to heapBefore; for
            # the observed/contract blocks we want absolute free-heap. On
            # desktop freeHeap() returns 0 (unlimited), so the offset is 0 and
            # we store 0 — which the runner treats as "no heap assertion".
            # max_alloc_block is absolute (0 = unlimited / desktop).
            observations[m["name"]] = {
                "tick_us": int(m["tick"]),
                "free_heap": max(0, heap),
                "max_alloc_block": int(m["block"]),
            }
    proc.wait()
    if proc.returncode != 0:
        # Scenario failed — don't persist observations from a failing run
        # (would record garbage as the latest reading).
        return proc.returncode

    if not observations:
        return 0

    # Explicit utf-8: scenario descriptions contain non-ASCII (→, ×, µ). On
    # Windows the default encoding is cp1252 and would mojibake those.
    with open(path, encoding="utf-8") as f:
        scenario = json.load(f)
    target = _host_target()
    today = datetime.date.today().isoformat()
    touched_observed = 0
    touched_contract = 0
    for step in scenario.get("steps", []):
        name = step.get("name")
        if name not in observations:
            continue
        # Always write observed.<target> — observations persist on every run.
        step.setdefault("observed", {})[target] = {
            "tick_us": observations[name]["tick_us"],
            "free_heap": observations[name]["free_heap"],
            "max_alloc_block": observations[name]["max_alloc_block"],
            "at": today,
        }
        touched_observed += 1
        # Only renegotiate the contract when --update-contract was passed. The
        # max_alloc_block field is *not* copied into the contract by default —
        # it's an opt-in floor that only a few scenarios assert (where LUT-fit
        # is part of the workload). Existing max_alloc_block contracts are
        # preserved if present.
        if update_contract:
            existing = step.get("contract", {}).get(target, {})
            new_block = {
                "tick_us": observations[name]["tick_us"],
                "free_heap": observations[name]["free_heap"],
                "set_by": today,
                "reason": update_reason or existing.get("reason", "updated"),
            }
            for k in ("tick_tolerance_pct", "heap_tolerance_pct", "tolerance_us",
                      "max_alloc_block"):
                if k in existing:
                    new_block[k] = existing[k]
            step.setdefault("contract", {})[target] = new_block
            touched_contract += 1

    if touched_observed or touched_contract:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(scenario, f, indent=2, ensure_ascii=False)
            f.write("\n")
        what = []
        if touched_observed:
            what.append(f"observed[{target}] × {touched_observed}")
        if touched_contract:
            what.append(f"contract[{target}] × {touched_contract}")
        print(f"  WROTE  {path.name} ({', '.join(what)})")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--name", default=None,
                        help="Scenario name (file stem). Runs all if omitted.")
    parser.add_argument("--module", default=None,
                        help="Module filter. Runs only scenarios that match.")
    parser.add_argument("--update-contract", action="store_true",
                        help=("Renegotiate the per-step performance contract: write "
                              "observed tick/heap into contract[<host-target>] and "
                              "stamp set_by + reason. Requires --reason."))
    parser.add_argument("--reason", default=None,
                        help=("Why the contract is being renegotiated (required with "
                              "--update-contract). Examples: 'tighter Layer LUT copy', "
                              "'accepted DMX driver overhead'."))
    args = parser.parse_args()

    if args.update_contract and not args.reason:
        parser.error("--update-contract requires --reason "
                     "(e.g. --reason 'tightened after Layer optimisation')")

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
        sys.exit(_run_one(scenario_file, args.update_contract, args.reason))

    if module_filter:
        paths = test_meta.paths_for_module(module_filter)
        if not paths:
            print(f"No scenarios found for module: {module_filter}")
            sys.exit(1)
        print(f"Module filter: {module_filter} ({len(paths)} scenario(s))")
        failed = sum(1 for p in paths if _run_one(p, args.update_contract, args.reason) != 0)
        sys.exit(1 if failed else 0)

    # Run all scenarios. We iterate per-file (instead of letting the C++ runner
    # auto-discover) because _run_one captures MEASURE lines and writes
    # observed.<target> blocks back into each scenario JSON on every run.
    paths = sorted((ROOT / "test" / "scenarios").rglob("scenario_*.json"))
    failed = sum(1 for p in paths if _run_one(p, args.update_contract, args.reason) != 0)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
