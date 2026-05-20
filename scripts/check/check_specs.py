#!/usr/bin/env python3
"""Check that implemented MoonModules have corresponding promoted specs.

Scans src/ for MoonModule .h files and verifies each has a matching
.md file in docs/moonmodules/. Reports missing or outdated specs.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SRC = ROOT / "src"
SPECS = ROOT / "docs" / "moonmodules"

# Map source directories to spec directories
SOURCE_DIRS = {
    "core": "core",
    "light": "light",
}

def find_moonmodules():
    """Find all .h files that define MoonModule subclasses."""
    modules = []
    for h_file in SRC.rglob("*.h"):
        # Skip platform, ui, and non-module headers
        rel = h_file.relative_to(SRC)
        if str(rel).startswith("platform/") or str(rel).startswith("ui/"):
            continue

        content = h_file.read_text()
        # Check if file defines a class inheriting from MoonModule or its subclasses
        if re.search(r'class\s+\w+\s*:\s*public\s+\w*(MoonModule|EffectBase|DriverBase|ModifierBase|LayoutBase)', content):
            # Skip abstract base classes (pure virtual methods + no controls)
            has_pure_virtual = re.search(r'=\s*0\s*;', content)
            has_controls = re.search(r'controls_\.add', content)
            if has_pure_virtual and not has_controls:
                continue
            modules.append(h_file)
    return modules

def find_spec(module_path):
    """Find the matching spec .md file for a source .h file."""
    name = module_path.stem  # e.g. "NoiseEffect"

    # Search all spec directories
    for md in SPECS.rglob("*.md"):
        if md.stem == name:
            return md
    return None

def check_spec_freshness(source_path, spec_path):
    """Check if spec mentions key elements from the source."""
    issues = []
    source = source_path.read_text()
    spec = spec_path.read_text()

    # Extract control names from source
    controls = re.findall(r'controls_\.add\w+\("(\w+)"', source)
    for ctrl in controls:
        if ctrl not in spec:
            issues.append(f"control '{ctrl}' not in spec")

    return issues

def main():
    modules = find_moonmodules()
    missing = []
    outdated = []
    ok = []

    for mod in sorted(modules):
        rel = mod.relative_to(SRC)
        spec = find_spec(mod)
        if not spec:
            missing.append(rel)
        else:
            issues = check_spec_freshness(mod, spec)
            if issues:
                outdated.append((rel, spec.relative_to(ROOT), issues))
            else:
                ok.append(rel)

    # Report
    print(f"Spec check: {len(modules)} modules, {len(ok)} ok, {len(missing)} missing, {len(outdated)} outdated")

    if missing:
        print("\nMissing specs:")
        for m in missing:
            print(f"  {m} — no .md in docs/moonmodules/")

    if outdated:
        print("\nOutdated specs:")
        for src, spec, issues in outdated:
            print(f"  {src} → {spec}")
            for issue in issues:
                print(f"    {issue}")

    if missing or outdated:
        print()
        sys.exit(1)
    else:
        print("All specs up to date.")

if __name__ == "__main__":
    main()
