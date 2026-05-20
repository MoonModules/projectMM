#!/usr/bin/env python3
"""Run scenario tests against a live device via HTTP.

Same JSON format as the in-process runner. Executes steps via REST API
and collects per-step performance measurements.
"""

import argparse
import json
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SCENARIOS_DIR = ROOT / "test" / "scenarios"
BASELINE_FILE = ROOT / "test" / "scenario-baseline.json"


class Client:
    def __init__(self, host: str):
        self.base = f"http://{host}"

    def get(self, path: str):
        req = urllib.request.Request(f"{self.base}{path}")
        with urllib.request.urlopen(req, timeout=5) as resp:
            return json.loads(resp.read())

    def post(self, path: str, data: dict):
        body = json.dumps(data).encode()
        req = urllib.request.Request(f"{self.base}{path}", data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=5) as resp:
            return json.loads(resp.read())

    def delete(self, path: str):
        req = urllib.request.Request(f"{self.base}{path}", method="DELETE")
        with urllib.request.urlopen(req, timeout=5) as resp:
            return json.loads(resp.read())


def collect_metrics(client: Client, settle_s: float = 1.5) -> dict:
    """Wait for system to settle, then collect metrics."""
    time.sleep(settle_s)
    return client.get("/api/system")


def run_scenario(client: Client, scenario_path: Path, settle_s: float = 1.5) -> dict:
    """Run a scenario against a live device and return results."""
    with open(scenario_path) as f:
        scenario = json.load(f)

    name = scenario.get("name", scenario_path.stem)
    print(f"\n=== Scenario: {name} ===")
    print(scenario.get("description", ""))

    results = {"name": name, "steps": [], "passed": True}

    # Collect baseline metrics before any steps
    baseline = collect_metrics(client, settle_s=0.5)
    print(f"\n  Baseline: tick={baseline.get('tickTimeUs', '?')}us (FPS={baseline.get('fps', '?')})  heap={baseline.get('freeHeap', '?')}")

    for step in scenario.get("steps", []):
        step_name = step.get("name", "?")
        op = step.get("op", "")
        step_result = {"name": step_name, "op": op}

        try:
            if op == "add_module":
                data = {"type": step["type"], "id": step.get("id", ""),
                        "parent_id": step.get("parent_id", "")}
                resp = client.post("/api/modules", data)
                step_result["status"] = "ok" if resp.get("ok") else "error"
                print(f"  +     {step.get('id', '?')} ({step['type']})")

            elif op == "set_control":
                data = {"module": step["id"], "control": step["key"],
                        "value": step["value"]}
                resp = client.post("/api/control", data)
                step_result["status"] = "ok" if resp.get("ok") else "error"
                print(f"  SET   {step.get('id', '?')}.{step.get('key', '?')} = {step.get('value', '?')}")

            elif op == "delete_module":
                resp = client.delete(f"/api/modules/{step['id']}")
                step_result["status"] = "ok" if resp.get("ok") else "error"
                print(f"  -     {step.get('id', '?')}")

            else:
                step_result["status"] = "skipped"
                print(f"  SKIP  {step_name} (unknown op: {op})")

        except urllib.error.HTTPError as e:
            # Read the JSON error body for a friendly message
            try:
                body = json.loads(e.read())
                msg = body.get("error", str(e))
            except Exception:
                msg = str(e)
            step_result["status"] = "error"
            step_result["error"] = msg
            if op == "add_module":
                print(f"  WARN  {step_name}: {msg}")
            else:
                print(f"  FAIL  {step_name}: {msg}")
                results["passed"] = False
        except Exception as e:
            step_result["status"] = "error"
            step_result["error"] = str(e)
            if op == "add_module":
                print(f"  WARN  {step_name}: {e}")
            else:
                print(f"  FAIL  {step_name}: {e}")
                results["passed"] = False

        # Measure after this step if requested
        if step.get("measure"):
            metrics = collect_metrics(client, settle_s)
            step_result["metrics"] = metrics
            tick_us = metrics.get("tickTimeUs", 0)
            fps = 1000000 // tick_us if tick_us > 0 else metrics.get("fps", 0)
            heap = metrics.get("freeHeap", 0)
            print(f"  MEASURE  tick={tick_us}us (FPS={fps})  heap={heap}")

            # Check bounds
            bounds = step.get("bounds", {})
            if "fps" in bounds:
                # Absolute minimum
                if "min" in bounds["fps"]:
                    min_fps = bounds["fps"]["min"]
                    if fps < min_fps:
                        print(f"  FAIL  fps {fps} < {min_fps}")
                        results["passed"] = False
                    else:
                        print(f"  PASS  fps >= {min_fps}")
                # Relative to baseline (percentage)
                if "min_pct" in bounds["fps"] and baseline.get("fps", 0) > 0:
                    min_pct = bounds["fps"]["min_pct"]
                    threshold = int(baseline["fps"] * min_pct / 100)
                    if fps < threshold:
                        print(f"  FAIL  fps {fps} < {threshold} ({min_pct}% of baseline {baseline['fps']})")
                        results["passed"] = False
                    else:
                        print(f"  PASS  fps {fps} >= {min_pct}% of baseline")

        results["steps"].append(step_result)

    # Summary
    print(f"\n---")
    if results["passed"]:
        print(f"PASSED")
    else:
        print(f"FAILED")

    return results


def load_baseline() -> dict:
    if BASELINE_FILE.exists():
        with open(BASELINE_FILE) as f:
            return json.load(f)
    return {}


def save_baseline(data: dict):
    with open(BASELINE_FILE, "w") as f:
        json.dump(data, f, indent=2)


def compare_baseline(results: dict, baseline: dict):
    """Compare results against baseline, report regressions."""
    name = results["name"]
    if name not in baseline:
        print(f"  No baseline for '{name}' — run with --update-baseline first")
        return

    base = baseline[name]
    regressions = []

    for step, base_step in zip(results.get("steps", []), base.get("steps", [])):
        if "metrics" not in step or "metrics" not in base_step:
            continue
        m = step["metrics"]
        bm = base_step["metrics"]

        step_name = step.get("name", "?")

        # FPS regression > 10%
        if bm.get("fps", 0) > 0 and m.get("fps", 0) > 0:
            pct = (bm["fps"] - m["fps"]) / bm["fps"] * 100
            if pct > 10:
                regressions.append(f"{step_name}: FPS dropped {pct:.0f}% ({bm['fps']} → {m['fps']})")

        # Heap regression > 10KB
        if bm.get("freeHeap", 0) > 0 and m.get("freeHeap", 0) > 0:
            delta = bm["freeHeap"] - m["freeHeap"]
            if delta > 10240:
                regressions.append(f"{step_name}: heap dropped {delta // 1024}KB")

    if regressions:
        print(f"\n  REGRESSIONS for '{name}':")
        for r in regressions:
            print(f"    {r}")
    else:
        print(f"\n  No regressions for '{name}'")


def main():
    parser = argparse.ArgumentParser(description="Run live scenario tests")
    parser.add_argument("--host", default="localhost:8080",
                        help="Device host:port (default: localhost:8080)")
    parser.add_argument("--name", default=None,
                        help="Scenario name (without .json). Runs all if omitted.")
    parser.add_argument("--settle", type=float, default=1.5,
                        help="Settle time in seconds between step and measurement")
    parser.add_argument("--update-baseline", action="store_true",
                        help="Save results as new baseline")
    parser.add_argument("--compare-baseline", action="store_true",
                        help="Compare results against stored baseline")
    args = parser.parse_args()

    client = Client(args.host)

    # Verify connection
    try:
        state = client.get("/api/state")
        module_count = len(state.get("modules", []))
        print(f"Connected to {args.host} ({module_count} modules)")
    except Exception as e:
        print(f"Cannot connect to {args.host}: {e}")
        sys.exit(1)

    # Find scenarios
    if args.name:
        paths = [SCENARIOS_DIR / f"{args.name}.json"]
    else:
        paths = sorted(SCENARIOS_DIR.glob("*.json"))

    if not paths:
        print("No scenarios found")
        sys.exit(1)

    # Run scenarios
    all_results = {}
    all_passed = True
    for path in paths:
        if not path.exists():
            print(f"Scenario not found: {path}")
            continue
        result = run_scenario(client, path, args.settle)
        all_results[result["name"]] = result
        if not result["passed"]:
            all_passed = False

    # Baseline
    if args.update_baseline:
        save_baseline(all_results)
        print(f"\nBaseline saved to {BASELINE_FILE}")

    if args.compare_baseline:
        baseline = load_baseline()
        for name, result in all_results.items():
            compare_baseline(result, baseline)

    print(f"\n=== {len(all_results)} scenario(s), "
          f"{'all passed' if all_passed else 'SOME FAILED'} ===")
    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()
