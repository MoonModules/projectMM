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

# Reuse the shared test-metadata parser so scenario discovery stays in one place.
sys.path.insert(0, str(ROOT / "scripts" / "docs"))
import _test_metadata as test_meta  # noqa: E402


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


def _today_iso() -> str:
    """ISO date stamp for set_by fields. Local timezone is fine — set_by is a
    coarse "around when did this contract get blessed" marker, not a timestamp."""
    import datetime
    return datetime.date.today().isoformat()


def collect_metrics(client: Client, settle_s: float = 1.5) -> dict:
    """Wait for system to settle, then collect metrics. Augments /api/system with
    `dynamicBytesTotal` (sum from the module tree) so callers can compare the
    model's allocation prediction against the observed free heap."""
    time.sleep(settle_s)
    metrics = client.get("/api/system")
    try:
        state = client.get("/api/state")
        metrics["dynamicBytesTotal"] = _sum_dynamic_bytes(state)
    except Exception:
        metrics["dynamicBytesTotal"] = None
    return metrics


def _control_value(module: dict, name: str):
    """Return the value of a named control on a module, or None."""
    for ctrl in module.get("controls", []):
        if ctrl.get("name") == name:
            return ctrl.get("value")
    return None


def count_lights(client: Client) -> int:
    """Derive the total light count from layout modules in the state tree.

    Layout modules expose width/height/depth controls; their product is the
    grid's light count. Used to scale the FPS-throughput floor to the grid.
    The module tree is nested (children[]), so walk it recursively.
    """
    def walk(module: dict) -> int:
        total = 0
        w = _control_value(module, "width")
        h = _control_value(module, "height")
        d = _control_value(module, "depth")
        if w is not None and h is not None and d is not None:
            try:
                total += int(w) * int(h) * int(d)
            except (ValueError, TypeError):
                print(f"  WARN  count_lights: non-numeric w/h/d "
                      f"({w!r}/{h!r}/{d!r}) on module {module.get('name','?')}, skipped")
        for child in module.get("children", []):
            total += walk(child)
        return total

    state = client.get("/api/state")
    return sum(walk(m) for m in state.get("modules", []))


def _detect_target(state: dict) -> str:
    """Identify the build target so per-step contract values can be looked up.

    ESP32: read SystemModule.board (`esp32`, `esp32-eth`, `esp32-eth-wifi`,
    `esp32s3-n16r8`, …) — set at compile time from MM_BOARD_NAME and exposed
    through the `board` control. Desktop: same key but reports `unknown`, so
    we substitute pc-<host-os> using the runtime os name (still distinguishes
    macOS vs Linux vs Windows builds, which can differ in tick noticeably).
    """
    import platform
    board = None
    for m in state.get("modules", []):
        if m.get("type") != "SystemModule":
            continue
        for c in m.get("controls", []):
            if c.get("name") == "board":
                board = c.get("value")
                break
        break
    if board and board != "unknown":
        return board
    # Desktop fallback
    osmap = {"Darwin": "pc-macos", "Linux": "pc-linux", "Windows": "pc-windows"}
    return osmap.get(platform.system(), "pc-unknown")


def _sum_dynamic_bytes(state: dict) -> int:
    """Sum dynamicBytes across the live module tree. Returned by /api/state per
    module; the sum is the model's prediction for how much heap the tree owns.

    NOT the same as (boot_heap - free_heap): the framework (lwIP, WiFi stack,
    FreeRTOS, HTTP server kernel buffers) consumes heap outside the model.
    Printed alongside the contract for sanity-checking — a regression here
    means a module started allocating something the contract didn't budget for.
    """
    total = 0
    def walk(modules):
        nonlocal total
        for m in modules:
            try:
                total += int(m.get("dynamicBytes", 0))
            except (TypeError, ValueError):
                pass
            walk(m.get("children", []))
    walk(state.get("modules", []))
    return total


def _collect_module_names(state: dict) -> set:
    """Collect every module name in the live tree, including nested children.
    Used by mutate scenarios to pre-flight that every id they touch is wired."""
    names = set()
    def walk(modules):
        for m in modules:
            n = m.get("name")
            if n:
                names.add(n)
            walk(m.get("children", []))
    walk(state.get("modules", []))
    return names


def run_scenario(client: Client, scenario_path: Path, settle_s: float = 1.5,
                 update_contract: bool = False,
                 update_reason: str | None = None) -> dict:
    """Run a scenario against a live device and return results.

    Mode handling (see docs/testing.md § Scenario modes):
      construct  — scenario builds the pipeline from scratch. Live device's
                   main.cpp owns the top-level shape, so construct scenarios
                   only run in-process. Skip here with a clear note.
      mutate     — scenario assumes a wired pipeline. Skip the fixture array
                   (the device IS the fixture) and run only the steps. Steps
                   that touch ids not present on the device hard-fail (instead
                   of the old WARN-and-continue which silently produced
                   meaningless passes).
    """
    with open(scenario_path) as f:
        scenario = json.load(f)

    name = scenario.get("name", scenario_path.stem)
    mode = scenario.get("mode", "construct")  # back-compat default
    print(f"\n=== Scenario: {name} ===")
    print(scenario.get("description", ""))

    results = {"name": name, "steps": [], "passed": True, "skipped": False}
    created_modules = []  # mutate scenarios rarely add modules but the existing cleanup path is still useful
    wrote_observations = [False]  # sentinel; flipped by each measure step that runs

    if mode == "construct":
        print(f"\n  SKIP (mode=construct — runs in-process only; the live device's "
              f"main.cpp owns the top-level shape)")
        results["skipped"] = True
        return results
    if mode != "mutate":
        print(f"\n  FAIL — unknown mode: {mode!r} (expected construct or mutate)")
        results["passed"] = False
        return results

    # Pre-flight: every id touched by the steps must already exist on the device.
    # A scenario in mutate mode is meant to tweak the live pipeline; a missing id
    # means the scenario was written against a different wiring than the device
    # has, and the old silent-skip path produced meaningless passes.
    target = "unknown"
    try:
        live_state = client.get("/api/state")
        target = _detect_target(live_state)
        live_names = _collect_module_names(live_state)
        # Include reset block ids in the pre-flight — a reset step that
        # references a missing module would silently no-op and the baseline
        # would measure unintended state.
        referenced = {step["id"] for step in scenario.get("steps", [])
                      if step.get("op") in ("set_control", "delete_module") and step.get("id")}
        referenced |= {r["id"] for r in scenario.get("reset", [])
                       if r.get("op") == "set_control" and r.get("id")}
        missing = sorted(referenced - live_names)
        if missing:
            print(f"\n  FAIL — mutate scenario references ids not on the live device: "
                  f"{', '.join(missing)}. Re-flash, or write the scenario against the "
                  f"actual wiring.")
            results["passed"] = False
            return results
    except Exception as e:
        print(f"\n  WARN — couldn't pre-flight live module names: {e}")
    print(f"  Target: {target}")
    results["target"] = target

    # Reset block: scenarios that mutate shared controls (Mirror toggles, grid
    # size, Preview detail, …) declare a `reset` array of set_control steps that
    # restores those controls to production defaults BEFORE the scenario runs.
    # Without this each scenario's measurements depend on whatever the previous
    # scenario left behind, so contract assertions become coupled to run order.
    reset_steps = scenario.get("reset", [])
    if reset_steps:
        print(f"\n  --- reset ({len(reset_steps)} steps) ---")
        for r_step in reset_steps:
            if r_step.get("op") != "set_control":
                continue
            try:
                client.post("/api/control", {
                    "module": r_step["id"],
                    "control": r_step["key"],
                    "value": r_step["value"]
                })
                print(f"  SET   {r_step.get('id','?')}.{r_step.get('key','?')} = {r_step.get('value','?')}")
            except Exception as e:
                print(f"  WARN  reset {r_step.get('name','?')}: {e}")

    # Collect baseline AFTER reset so it reflects the normalized state.
    baseline = collect_metrics(client, settle_s=settle_s)
    print(f"\n  Baseline: tick={baseline.get('tickTimeUs', '?')}us (FPS={baseline.get('fps', '?')})  heap={baseline.get('freeHeap', '?')}")

    # Live runs `steps` only — `fixture` is the in-process equivalent of what
    # main.cpp already wired on the device.
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
                if resp.get("note") == "already exists":
                    print(f"  =     {step.get('id', '?')} (exists)")
                else:
                    print(f"  +     {step.get('id', '?')} ({step['type']})")
                    created_modules.append(step.get("id", ""))

            elif op == "set_control":
                data = {"module": step["id"], "control": step["key"],
                        "value": step["value"]}
                resp = client.post("/api/control", data)
                step_result["status"] = "ok" if resp.get("ok") else "error"
                print(f"  SET   {step.get('id', '?')}.{step.get('key', '?')} = {step.get('value', '?')}")
                # If this step doesn't measure (so `collect_metrics` won't wait
                # for us), still give the device a moment — a set_control that
                # triggers buildState briefly mutates the module tree, and the
                # very next API call can hit a transient "module not found".
                # 500 ms is empirically enough on the Olimex; cheap insurance.
                if not (step.get("measure") or op == "measure"):
                    time.sleep(0.5)

            elif op == "delete_module":
                resp = client.delete(f"/api/modules/{step['id']}")
                step_result["status"] = "ok" if resp.get("ok") else "error"
                print(f"  -     {step.get('id', '?')}")

            elif op == "measure":
                # Pure measurement step (introduced for the build-up scenario shape).
                # No REST call; the measure block below picks it up via step["measure"]
                # or the implicit-measure clause we add to the same dispatcher.
                step_result["status"] = "ok"

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
            # Every rejected step is a real failure. The old policy WARN'd on
            # add_module which silently turned "top-level rejected" into a
            # missing test step — meaningless passes. Mutate scenarios shouldn't
            # add top-level anyway; if they do, treat it as a scenario bug.
            print(f"  FAIL  {step_name}: {msg}")
            results["passed"] = False
        except Exception as e:
            step_result["status"] = "error"
            step_result["error"] = str(e)
            print(f"  FAIL  {step_name}: {e}")
            results["passed"] = False

        # Measure after this step if requested (explicit "measure": true OR op == "measure").
        # Skip the measurement block when the step itself failed: writing observed
        # values from a failed-step state would persist garbage as the "latest
        # reading" and the contract assertion would compare against an
        # untrustworthy measurement.
        if step_result.get("status") == "error":
            results["steps"].append(step_result)
            continue
        if step.get("measure") or op == "measure":
            metrics = collect_metrics(client, settle_s)
            step_result["metrics"] = metrics
            tick_us = metrics.get("tickTimeUs", 0)
            fps = 1000000 // tick_us if tick_us > 0 else metrics.get("fps", 0)
            heap = metrics.get("freeHeap", 0)
            max_block = metrics.get("maxBlock", 0)
            model_bytes = metrics.get("dynamicBytesTotal")
            print(f"  MEASURE  tick={tick_us}us (FPS={fps})  heap={heap}  "
                  f"block={max_block}  model={model_bytes}")

            # Per-step contract: { "contract": { "<target>": { "tick_us": N,
            #   "free_heap": M, "tick_tolerance_pct": P, "heap_tolerance_pct": Q,
            #   "set_by": "YYYY-MM-DD", "reason": "..." } } }
            # Contracts are hand-set promises — see docs/testing.md § Performance
            # contracts. `--update-contract --reason "..."` rewrites them.
            contract_block = step.get("contract", {}).get(target) if step.get("contract") else None
            if contract_block:
                # Defaults reflect run-to-run variance, not "I don't care":
                #   pc-*       — multi-process OS jitter, 20% pct + 200us absolute
                #                floor. The floor dominates below ~1ms tick (the
                #                realistic case for PC scenarios today).
                #   esp32-*    — bounded RTOS but lwIP/EMAC jitter, 10% pct + 5us
                #                absolute floor.
                is_pc = target.startswith("pc-")
                tick_tol_pct = contract_block.get("tick_tolerance_pct",
                                                  20 if is_pc else 10)
                heap_tol_pct = contract_block.get("heap_tolerance_pct",
                                                  20 if is_pc else 10)
                tol_us_abs = contract_block.get("tolerance_us", 200 if is_pc else 5)
                exp_tick = contract_block.get("tick_us")
                exp_heap = contract_block.get("free_heap")
                if exp_tick is not None and exp_tick > 0:
                    # tick contract is a *ceiling* — faster than contract is good
                    # news (mirror of heap being a floor). Tolerance absorbs
                    # upward jitter only; speedups never fail.
                    overshoot = tick_us - exp_tick
                    allowed = max(exp_tick * tick_tol_pct / 100.0, tol_us_abs)
                    if overshoot <= 0:
                        print(f"  PASS  tick {tick_us}us <= contract {exp_tick}us "
                              f"(margin {-overshoot:.0f}us)")
                    elif overshoot > allowed:
                        print(f"  FAIL  tick {tick_us}us vs contract {exp_tick}us "
                              f"(over by {overshoot:.0f}us > allowed {allowed:.0f}us)")
                        results["passed"] = False
                    else:
                        print(f"  PASS  tick {tick_us}us vs contract {exp_tick}us "
                              f"(over by {overshoot:.0f}us within {allowed:.0f}us)")
                if exp_heap is not None and exp_heap > 0:
                    # Contract is a *floor* — the device must deliver at least this
                    # much free heap. More is better; less by more than tolerance is
                    # a regression. Tolerance applies because of legitimate run-to-
                    # run drift in lwIP/TCP buffer pools.
                    drop_pct = (exp_heap - heap) * 100.0 / exp_heap if heap < exp_heap else 0
                    if drop_pct > heap_tol_pct:
                        print(f"  FAIL  free_heap {heap} dropped {drop_pct:.1f}% "
                              f"below contract {exp_heap}")
                        results["passed"] = False
                    else:
                        print(f"  PASS  free_heap {heap} >= contract {exp_heap} "
                              f"(within -{heap_tol_pct}% tolerance)")
                # max_alloc_block contract is also a *floor* — opt-in per scenario.
                # The LUT/buffer allocators need a single contiguous chunk; on a
                # fragmented heap the largest block can be much smaller than free
                # heap, and Layer silently degrades to 1:1 (mirror disappears) when
                # the LUT won't fit. Scenarios that depend on that allocation
                # succeeding assert a minimum block here.
                exp_block = contract_block.get("max_alloc_block")
                if exp_block is not None and exp_block > 0 and max_block > 0:
                    drop_pct = (exp_block - max_block) * 100.0 / exp_block if max_block < exp_block else 0
                    if drop_pct > heap_tol_pct:
                        print(f"  FAIL  max_alloc_block {max_block} dropped {drop_pct:.1f}% "
                              f"below contract {exp_block}")
                        results["passed"] = False
                    else:
                        print(f"  PASS  max_alloc_block {max_block} >= contract {exp_block} "
                              f"(within -{heap_tol_pct}% tolerance)")

            # observed.<target>: the *observation*, written on every run so the
            # JSON diff shows drift even when scenarios still pass the contract.
            # No --reason needed (observations aren't promises). Date-only stamp
            # keeps churn to one diff per day max.
            step.setdefault("observed", {})[target] = {
                "tick_us": int(tick_us),
                "free_heap": int(heap),
                "max_alloc_block": int(max_block),
                "at": _today_iso(),
            }
            wrote_observations[0] = True

            # --update-contract: rewrite the contract in the scenario JSON for the
            # active target. This is *renegotiating* a contract, not refreshing a
            # last-reading baseline — set_by + reason are stamped so the diff
            # records when and why the promise changed. Caller is responsible for
            # committing the diff intentionally.
            if update_contract:
                # Preserve any per-step tolerance overrides already in place.
                existing = step.get("contract", {}).get(target, {})
                new_block = {
                    "tick_us": int(tick_us),
                    "free_heap": int(heap),
                    "set_by": _today_iso(),
                    "reason": update_reason or existing.get("reason", "updated"),
                }
                for k in ("tick_tolerance_pct", "heap_tolerance_pct", "tolerance_us",
                          "max_alloc_block"):
                    if k in existing:
                        new_block[k] = existing[k]
                step.setdefault("contract", {})[target] = new_block

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
                # FPS×lights throughput floor — compared against the measured
                # tick *time* (the device's native unit), not derived FPS.
                # Per-grid budget: max_tick_us = lights * 1e6 / product.
                if "min_fps_led_product" in bounds["fps"]:
                    product = bounds["fps"]["min_fps_led_product"]
                    lights = count_lights(client)
                    if not isinstance(product, (int, float)) or product <= 0:
                        print(f"  WARN  min_fps_led_product: invalid value "
                              f"{product!r}, skipped")
                    elif lights > 0 and tick_us > 0:
                        max_tick = round(lights * 1_000_000 / product)
                        if tick_us > max_tick:
                            print(f"  FAIL  tick {tick_us}us > {max_tick}us "
                                  f"(throughput budget for {lights} lights)")
                            results["passed"] = False
                        else:
                            print(f"  PASS  tick {tick_us}us <= {max_tick}us ({lights} lights)")
                    else:
                        print("  WARN  min_fps_led_product: no layout lights / tick, skipped")

        results["steps"].append(step_result)

    # Cleanup: delete modules that were created by this scenario
    for module_id in reversed(created_modules):
        try:
            client.delete(f"/api/modules/{module_id}")
            print(f"  -     {module_id} (cleanup)")
        except Exception:
            pass

    # Write the scenario JSON back if anything changed:
    #   - observed.<target> was updated by any measure step (every run); OR
    #   - --update-contract renegotiated the contract.
    # JSON dumps in a stable shape (2-space indent, trailing newline) so the
    # diff stays readable.
    if wrote_observations[0] or update_contract:
        with open(scenario_path, "w") as f:
            json.dump(scenario, f, indent=2, ensure_ascii=False)
            f.write("\n")
        what = []
        if wrote_observations[0]:
            what.append(f"observed[{target}]")
        if update_contract:
            what.append(f"contract[{target}]")
        print(f"  WROTE  {scenario_path.name} ({' + '.join(what)})")

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
    parser.add_argument("--module", default=None,
                        help="Filter to scenarios whose top-level module / also matches.")
    parser.add_argument("--settle", type=float, default=3.0,
                        help="Settle time in seconds between step and measurement")
    parser.add_argument("--update-baseline", action="store_true",
                        help="Save results as new baseline")
    parser.add_argument("--compare-baseline", action="store_true",
                        help="Compare results against stored baseline")
    parser.add_argument("--update-contract", action="store_true",
                        help=("Renegotiate the per-step performance contract: write "
                              "observed tick/heap into contract[<target>] and stamp "
                              "set_by + reason. Requires --reason. Overwrites existing "
                              "values for the active target only; other targets untouched."))
    parser.add_argument("--reason", default=None,
                        help=("Why the contract is being renegotiated (required with "
                              "--update-contract). Examples: 'tighter Layer LUT copy', "
                              "'accepted DMX driver overhead'. Written into each updated "
                              "contract block."))
    args = parser.parse_args()

    if args.update_contract and not args.reason:
        parser.error("--update-contract requires --reason "
                     "(e.g. --reason 'tightened after Layer optimisation')")

    client = Client(args.host)

    # Verify connection
    try:
        state = client.get("/api/state")
        module_count = len(state.get("modules", []))
        print(f"Connected to {args.host} ({module_count} modules)")
    except Exception as e:
        print(f"Cannot connect to {args.host}: {e}")
        sys.exit(1)

    # Find scenarios via the shared metadata module (recursive: scenarios live under core/, light/, …)
    if args.name:
        match = test_meta.find_scenario_path(args.name)
        if not match:
            print(f"Scenario not found: {args.name}.json under {test_meta.SCENARIO_DIR}")
            sys.exit(1)
        paths = [match]
    else:
        paths = [s["path"] for s in test_meta.collect_scenario_files()]

    if args.module and args.module.lower() != "all":
        module_paths = set(test_meta.paths_for_module(args.module))
        filtered = [p for p in paths if p in module_paths]
        if not filtered:
            print(f"No scenarios match module: {args.module}")
            sys.exit(1)
        paths = filtered
        print(f"Module filter: {args.module} ({len(paths)} scenario(s))")

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
        result = run_scenario(client, path, args.settle,
                              update_contract=args.update_contract,
                              update_reason=args.reason)
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
