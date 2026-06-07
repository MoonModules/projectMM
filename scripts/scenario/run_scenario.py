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
sys.path.insert(0, str(ROOT / "scripts" / "scenario"))
import _observed  # noqa: E402

_HOST = {"darwin": "macos", "win32": "windows"}.get(sys.platform, "linux")


def _resolve_runner() -> Path:
    """Find mm_scenarios. MSVC multi-config drops it in test/Release/; Ninja and
    single-config generators drop it in test/. Check both so the script works
    with either layout."""
    base = ROOT / "build" / _HOST / "test" / "mm_scenarios"
    suffix = ".exe" if sys.platform == "win32" else ""
    candidates = [base.with_suffix(suffix)]
    if sys.platform == "win32":
        candidates.insert(0, base.parent / "Release" / f"mm_scenarios{suffix}")
    for c in candidates:
        if c.exists():
            return c
    return candidates[-1]  # fall through with the simplest path for the error message


RUNNER = _resolve_runner()

# Format emitted by scenario_runner.cpp's measure block:
#   MEASURE <step-name>: tick=Nus FPS=N lights=N heap=N (step: ±N) block=N
# `<step-name>` may contain hyphens and underscores. heap is the absolute free
# heap after the measurement (what observed.free_heap consumes); the (step: ±N)
# fragment is a human-readable delta for diagnostics — not captured here.
# Both heap and block are 0 on desktop where the platform stubs return
# "unlimited".
_MEASURE_RE = re.compile(
    r"^\s*MEASURE\s+(?P<name>\S+):\s+tick=(?P<tick>\d+)us\s+FPS=\d+\s+lights=\d+"
    r"\s+heap=(?P<heap>\d+).*?\bblock=(?P<block>\d+)"
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
            # heap is absolute free-heap after measurement (the value the
            # observed/contract blocks consume directly). On desktop
            # freeHeap() returns 0 (unlimited) which the runner treats as
            # "no heap assertion". max_alloc_block is also absolute.
            observations[m["name"]] = {
                "tick_us": int(m["tick"]),
                "free_heap": int(m["heap"]),
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
        # observed.<target> stores a rolling [min, max] range per scalar that
        # only widens when a fresh measurement falls outside the current bounds
        # — drops JSON churn on routine runs to near-zero while preserving full
        # drift visibility. When --update-contract was passed, reset the range
        # to the current single point (the historical range was for the
        # previous contract). See scripts/scenario/_observed.py.
        existing_obs = step.get("observed", {}).get(target)
        if update_contract:
            new_obs = _observed.reset(observations[name], today)
            obs_changed = True
        else:
            new_obs, obs_changed = _observed.widen(existing_obs, observations[name], today)
        if obs_changed:
            step.setdefault("observed", {})[target] = new_obs
            touched_observed += 1

        # Only renegotiate the contract when --update-contract was passed. The
        # max_alloc_block field is *not* added unconditionally — it's an opt-in
        # floor that only a few scenarios assert (where LUT-fit is part of the
        # workload). When the existing contract already opts in, refresh the
        # value from the current run (consistent with tick_us / free_heap);
        # tolerances are user-set knobs and stay as-is.
        if update_contract:
            existing = step.get("contract", {}).get(target, {})
            new_block = {
                "tick_us": observations[name]["tick_us"],
                "free_heap": observations[name]["free_heap"],
                "set_by": today,
                "reason": update_reason or existing.get("reason", "updated"),
            }
            for k in ("tick_tolerance_pct", "heap_tolerance_pct", "tolerance_us"):
                if k in existing:
                    new_block[k] = existing[k]
            # max_alloc_block: opt-in (only carry it over if the existing
            # contract had it), but refresh the value rather than copying
            # the stale one. observations[name] always carries this key
            # because the scenario runner emits it; if it ever doesn't,
            # fall back to the existing value so we don't drop the opt-in.
            if "max_alloc_block" in existing:
                new_block["max_alloc_block"] = observations[name].get(
                    "max_alloc_block", existing["max_alloc_block"])
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
