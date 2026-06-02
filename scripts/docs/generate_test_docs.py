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
        "update the JSON file's top-level `module` / `also` / `description` "
        "and per-step `description` fields instead, then regenerate."
    )
    lines.append("")
    lines.append(
        "Scenario tests are the integration tier in the [test strategy](../testing.md): "
        "each one is a JSON script that drives the full pipeline (PC or live ESP32) "
        "and captures bounded FPS / heap measurements per step. "
        "Run them with `scripts/scenario/run_scenario.py` (PC) or "
        "`scripts/scenario/run_live_scenario.py` (live device)."
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
            if f["also"]:
                lines.append("")
                lines.append(f"*Also touches: {', '.join(f['also'])}.*")
            lines.append("")
            for name, desc, op in f["steps"]:
                bullet = desc if desc else f"_{name}_ ({op})"
                lines.append(f"- **{name}** ({op}) — {bullet}" if desc else f"- **{name}** ({op})")
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
