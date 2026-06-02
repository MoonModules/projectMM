#!/usr/bin/env python3
"""Generate docs/tests/unit-tests.md and docs/tests/scenario-tests.md from the source of truth.

Unit tests: walks test/unit/ recursively for unit_*.cpp, extracts `// @module <Name>`, optional
`// @also A, B`, and a single `//` description line above each `TEST_CASE("...")`.

Scenarios: walks test/scenarios/scenario_*.json, reads top-level `module`,
`also`, `name`, `description`, and per-step `description`.

Both outputs are grouped by primary `@module` / `module`. The script is the
single owner of the generated files — running it idempotently produces the
same bytes (verified by --check).

Usage:
    uv run scripts/docs/generate_test_docs.py             # writes both files
    uv run scripts/docs/generate_test_docs.py --unit      # unit-tests.md only
    uv run scripts/docs/generate_test_docs.py --scenario  # scenario-tests.md only
    uv run scripts/docs/generate_test_docs.py --check     # exit 1 if regen would change files
"""

import argparse
import sys
from collections import defaultdict
from pathlib import Path

from _test_metadata import (
    ROOT,
    collect_scenario_files,
    collect_unit_files,
)

OUT_DIR = ROOT / "docs" / "tests"
UNIT_OUT = OUT_DIR / "unit-tests.md"
SCENARIO_OUT = OUT_DIR / "scenario-tests.md"


def render_unit_tests(files: list[dict]) -> str:
    by_module: dict[str, list[dict]] = defaultdict(list)
    for f in files:
        by_module[f["module"] or "Uncategorized"].append(f)
    for mods in by_module.values():
        mods.sort(key=lambda f: f["path"].name)

    lines: list[str] = []
    lines.append("# Unit Tests")
    lines.append("")
    lines.append(
        "Auto-generated from `test/unit/{core,light}/unit_*.cpp` by `scripts/docs/generate_test_docs.py`. "
        "**Do not edit by hand** — update the source file's `@module` / `@also` "
        "and per-TEST_CASE `//` descriptions instead, then regenerate."
    )
    lines.append("")
    lines.append(
        "Unit tests are the fastest tier in the [test strategy](../testing.md): "
        "they run the production code in-process with doctest, no platform, no network. "
        "Each section below covers one module."
    )
    lines.append("")

    for module in sorted(by_module.keys()):
        lines.append(f"## {module}")
        lines.append("")
        for f in by_module[module]:
            rel = f["path"].relative_to(ROOT).as_posix()
            if f["file_description"]:
                lines.append(f"`{rel}` — {f['file_description']}")
            else:
                lines.append(f"`{rel}`")
            if f["also"]:
                lines.append(f"*Also touches: {', '.join(f['also'])}.*")
            lines.append("")
            for name, desc in f["cases"]:
                bullet = desc if desc else f"_{name}_"
                lines.append(f"- {bullet}")
            lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def _fmt_us(us: int) -> str:
    """Pretty-print microseconds with a friendly unit (us / ms / s)."""
    if us >= 1_000_000:
        return f"{us / 1_000_000:.2f}s"
    if us >= 1_000:
        return f"{us / 1_000:.1f}ms"
    return f"{us}µs"


def _fps_from_us(us: int) -> str:
    """Convert tick_us → frames-per-second string (no decimals; for headline display)."""
    if us <= 0:
        return "—"
    fps = 1_000_000 / us
    if fps >= 100:
        return f"{int(round(fps)):,}"
    return f"{fps:.1f}"


def _fmt_heap(bytes_: int) -> str:
    """Pretty-print free-heap bytes (KB once over 1024). 0 = unlimited (desktop)."""
    if bytes_ == 0:
        return "unlimited"
    if bytes_ >= 1024:
        return f"{bytes_ / 1024:.0f}KB"
    return f"{bytes_}B"


def _format_contract_line(target: str, c: dict) -> str:
    """One bullet line summarising a per-target contract."""
    parts = []
    if c.get("tick_us"):
        parts.append(f"tick ≤ {_fmt_us(int(c['tick_us']))} ({_fps_from_us(int(c['tick_us']))} FPS)")
    if c.get("free_heap"):
        parts.append(f"heap ≥ {_fmt_heap(int(c['free_heap']))}")
    if c.get("max_alloc_block"):
        parts.append(f"block ≥ {_fmt_heap(int(c['max_alloc_block']))}")
    if not parts:
        return f"`{target}`: (no thresholds)"
    extra = []
    if c.get("set_by"):
        extra.append(f"set {c['set_by']}")
    if c.get("reason"):
        extra.append(f"\"{c['reason']}\"")
    tail = f" — {' · '.join(extra)}" if extra else ""
    return f"`{target}`: " + " · ".join(parts) + tail


def _format_observed_line(target: str, o: dict) -> str:
    """One bullet line summarising a per-target observation."""
    parts = []
    if o.get("tick_us"):
        parts.append(f"tick {_fmt_us(int(o['tick_us']))} ({_fps_from_us(int(o['tick_us']))} FPS)")
    if o.get("free_heap"):
        parts.append(f"heap {_fmt_heap(int(o['free_heap']))}")
    if o.get("max_alloc_block"):
        parts.append(f"block {_fmt_heap(int(o['max_alloc_block']))}")
    tail = f" — observed {o['at']}" if o.get("at") else ""
    return f"`{target}`: " + " · ".join(parts) + tail


def _format_bounds(b: dict) -> list[str]:
    """One bullet per bound expressed in the JSON."""
    out: list[str] = []
    fps = b.get("fps") or {}
    if "min" in fps:
        out.append(f"FPS ≥ {fps['min']} (absolute)")
    if "min_pct" in fps:
        out.append(f"FPS ≥ {fps['min_pct']}% of baseline")
    if "min_fps_led_product" in fps:
        out.append(f"FPS × lights ≥ {fps['min_fps_led_product']:,}")
    heap = b.get("heap") or {}
    if "max_delta_bytes" in heap:
        out.append(f"heap growth ≤ {heap['max_delta_bytes']}B vs previous measure step")
    return out


def render_scenarios(files: list[dict]) -> str:
    by_module: dict[str, list[dict]] = defaultdict(list)
    for f in files:
        by_module[f["module"] or "Uncategorized"].append(f)
    for scens in by_module.values():
        scens.sort(key=lambda f: f["path"].name)

    lines: list[str] = []
    lines.append("# Scenario Tests")
    lines.append("")
    lines.append(
        "Auto-generated from `test/scenarios/{core,light}/scenario_*.json` by "
        "`scripts/docs/generate_test_docs.py`. **Do not edit by hand** — "
        "update the JSON file's top-level fields and per-step `description` "
        "/ `bounds` / `contract` / `observed` instead, then regenerate."
    )
    lines.append("")
    lines.append(
        "Scenario tests are the integration tier in the [test strategy](../testing.md): "
        "each one is a JSON script that drives the full pipeline (PC or live ESP32) "
        "and captures tick / heap per step against per-target contracts. "
        "Run them with `scripts/scenario/run_scenario.py` (PC) or "
        "`scripts/scenario/run_live_scenario.py` (live device). "
        "See [testing.md § Performance contracts](../testing.md#performance-contracts-contracttarget) "
        "for the contract semantics."
    )
    lines.append("")

    for module in sorted(by_module.keys()):
        lines.append(f"## {module}")
        lines.append("")
        for f in by_module[module]:
            rel = f["path"].relative_to(ROOT).as_posix()
            lines.append(f"### {f['name']}")
            lines.append("")
            lines.append(f"`{rel}` — {f['description']}")
            lines.append("")
            # Top-level scenario flags worth surfacing.
            meta_bits: list[str] = [f"**Mode**: `{f['mode']}`"]
            if f["live_only"]:
                meta_bits.append("**live-only** (skipped in-process)")
            if f["also"]:
                meta_bits.append(f"**Also touches**: {', '.join(f['also'])}")
            lines.append(" · ".join(meta_bits))
            lines.append("")
            # Per-step expansion. Only measured steps get a `####` heading;
            # un-measured prep steps (set_control without `measure: true`)
            # collapse to a "Setup:" bullet list under the next measured step,
            # since they carry no contract/observed data of their own and
            # rendering each one as a heading bloats the page without signal.
            prep_buffer: list[dict] = []
            for step in f["steps"]:
                if not step["measure"]:
                    prep_buffer.append(step)
                    continue
                # Flush: render this measured step with any preceding prep.
                name = step["name"]
                op = step["op"]
                lines.append(f"#### `{name}` ({op})  📏")
                lines.append("")
                if step["description"]:
                    lines.append(step["description"])
                    lines.append("")
                if prep_buffer:
                    lines.append("**Setup** (preceding non-measured steps):")
                    for p in prep_buffer:
                        bits = [f"`{p['name']}` ({p['op']})"]
                        if p["description"]:
                            bits.append(p["description"])
                        lines.append(f"- {' — '.join(bits)}")
                    lines.append("")
                    prep_buffer = []
                bounds = _format_bounds(step["bounds"])
                if bounds:
                    lines.append("**Bounds**:")
                    for b in bounds:
                        lines.append(f"- {b}")
                    lines.append("")
                if step["contract"]:
                    lines.append("**Contract** (tick is a ceiling, heap is a floor):")
                    for tgt in sorted(step["contract"].keys()):
                        lines.append(f"- {_format_contract_line(tgt, step['contract'][tgt])}")
                    lines.append("")
                if step["observed"]:
                    lines.append("**Observed** (latest reading per target):")
                    for tgt in sorted(step["observed"].keys()):
                        lines.append(f"- {_format_observed_line(tgt, step['observed'][tgt])}")
                    lines.append("")
            # Trailing prep steps after the last measurement (rare) get their
            # own collapsed bullet list under a "Trailing setup" header.
            if prep_buffer:
                lines.append("#### Trailing setup (no measurement after)")
                lines.append("")
                for p in prep_buffer:
                    bits = [f"`{p['name']}` ({p['op']})"]
                    if p["description"]:
                        bits.append(p["description"])
                    lines.append(f"- {' — '.join(bits)}")
                lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def write_or_check(path: Path, content: str, check_only: bool) -> bool:
    """Return True if the file matches `content` (or was written successfully)."""
    if check_only:
        if not path.exists():
            print(f"  MISSING  {path.relative_to(ROOT)}")
            return False
        if path.read_text() != content:
            print(f"  CHANGED  {path.relative_to(ROOT)}")
            return False
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    print(f"  WROTE    {path.relative_to(ROOT)}")
    return True


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--unit", action="store_true", help="Write only unit-tests.md")
    p.add_argument("--scenario", action="store_true", help="Write only scenario-tests.md")
    p.add_argument("--check", action="store_true",
                   help="Exit 1 if regeneration would change content on disk")
    args = p.parse_args()

    do_unit = args.unit or not args.scenario
    do_scenario = args.scenario or not args.unit

    ok = True
    if do_unit:
        content = render_unit_tests(collect_unit_files())
        if not write_or_check(UNIT_OUT, content, args.check):
            ok = False
    if do_scenario:
        content = render_scenarios(collect_scenario_files())
        if not write_or_check(SCENARIO_OUT, content, args.check):
            ok = False

    if args.check and not ok:
        print("Run scripts/docs/generate_test_docs.py to regenerate.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
