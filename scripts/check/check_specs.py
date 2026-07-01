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

        # Explicit utf-8: source files contain non-ASCII (→, µ, ×). Windows'
        # default read_text encoding is cp1252 and rejects those bytes.
        content = h_file.read_text(encoding="utf-8")
        # Check if file defines a class inheriting from MoonModule or its subclasses
        if re.search(r'class\s+\w+\s*:\s*public\s+\w*(MoonModule|EffectBase|DriverBase|ModifierBase|LayoutBase)', content):
            # Skip abstract base classes (pure virtual methods + no controls)
            has_pure_virtual = re.search(r'=\s*0\s*;', content)
            has_controls = re.search(r'controls_\.add', content)
            if has_pure_virtual and not has_controls:
                continue
            # Skip CRTP template bases (e.g. ParallelLedDriver<Derived>): a
            # `template<...> class X : public DriverBase` is shared infrastructure,
            # not a registered module — its controls belong to the concrete derived
            # classes, which carry the docs. The concrete subclass is `class Foo :
            # public X<Foo>` (no leading `template<`), so it is still picked up.
            if re.search(r'template\s*<[^>]*>\s*class\s+\w+\s*:\s*public\s+\w*'
                         r'(MoonModule|EffectBase|DriverBase|ModifierBase|LayoutBase)',
                         content):
                continue
            modules.append(h_file)
    return modules

# Effects, modifiers, and the leaf layouts (Grid/Sphere/Wheel) document themselves as one
# compact-row page per type rather than a file per module (docs consolidation — see the
# folder-structure decision). A module of one of these types is documented on its shared page;
# every other module keeps a per-module page named for the type. Keyed by type-name suffix so a
# new effect/modifier/layout/driver is folded in automatically. (Layouts/Layers/Drivers are
# CONTAINERS, not leaf modules — "Drivers" does not end in "Driver", so the container keeps its own
# per-module page; the CRTP base ParallelLedDriver is skipped in discover_modules as a template.)
CONSOLIDATED_PAGES = {
    "Effect": SPECS / "light" / "effects" / "effects.md",
    "Modifier": SPECS / "light" / "modifiers" / "modifiers.md",
    "Layout": SPECS / "light" / "layouts" / "layouts.md",
    "Driver": SPECS / "light" / "drivers" / "drivers.md",
}


def find_spec(module_path):
    """Find the matching spec .md for a source .h.

    A module whose type name ends in Effect/Modifier/Layout is documented on the shared
    per-type page (its row must carry every control name — checked by check_spec_freshness).
    Everything else uses the per-module page (stem match), as before.
    """
    name = module_path.stem  # e.g. "NoiseEffect"

    # MoonLive is a live-script module under light/moonlive/, not a normal effect — it keeps its
    # own page despite the *Effect suffix. Only the per-type folders consolidate.
    in_moonlive = "moonlive" in module_path.parts
    if not in_moonlive:
        for suffix, page in CONSOLIDATED_PAGES.items():
            if name.endswith(suffix):
                return page if page.exists() else None

    # Per-module page: match by filename stem anywhere under docs/moonmodules/.
    for md in SPECS.rglob("*.md"):
        if md.stem == name:
            return md
    return None

def check_spec_freshness(source_path, spec_path):
    """Check if spec mentions key elements from the source."""
    issues = []
    source = source_path.read_text(encoding="utf-8")
    spec = spec_path.read_text(encoding="utf-8")

    # Extract control names from source
    controls = re.findall(r'controls_\.add\w+\("(\w+)"', source)
    for ctrl in controls:
        if ctrl not in spec:
            issues.append(f"control '{ctrl}' not in spec")

    return issues

def check_source_links():
    """Verify every spec page carries a '## Source' section whose links resolve.

    The Source section links each spec back to the .h/.cpp (or UI/asset files)
    it documents. This pass catches two drifts the source->spec checks above
    can't: a source file renamed/moved out from under a spec's link, and a new
    spec page added without a Source section at all. Walks spec->source (the
    inverse direction), so it covers every .md including the few that document
    multiple files (ui.md, LightConfig.md) with no single matching module.

    Returns a list of (spec_rel, issue) tuples — empty when all is well.
    """
    issues = []
    for md in sorted(SPECS.rglob("*.md")):
        spec_rel = md.relative_to(ROOT)
        text = md.read_text(encoding="utf-8")
        # Capture only the Source section body — stop at the next top-level
        # header or end-of-file, so links in a later section aren't mistaken
        # for source links if a page ever has one after Source.
        m = re.search(r'^## Source\s*$(.*?)(?=^##\s|\Z)', text, re.MULTILINE | re.DOTALL)
        if not m:
            issues.append((spec_rel, "no '## Source' section"))
            continue
        # Markdown links in the Source section: [label](relative/path).
        # Only check relative paths (the repo convention); skip any http(s) URL.
        links = re.findall(r'\]\(([^)]+)\)', m.group(1))
        rel_links = [href for href in links if not href.startswith(("http://", "https://"))]
        if not rel_links:
            issues.append((spec_rel, "'## Source' section has no relative source link"))
            continue
        for href in rel_links:
            target = href.split("#", 1)[0]  # drop any anchor
            # A source link must resolve to a file inside the repo. Relative
            # `../` hops are expected (specs are nested under docs/); what's
            # rejected is an absolute path or one whose resolved target lands
            # outside the repo root.
            candidate = (md.parent / target).resolve()
            if target.startswith("/") or not candidate.is_relative_to(ROOT):
                issues.append((spec_rel, f"source link escapes repo or is absolute: {href}"))
            elif not candidate.exists():
                issues.append((spec_rel, f"source link does not resolve: {href}"))
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

    source_link_issues = check_source_links()

    # Report
    print(f"Spec check: {len(modules)} modules, {len(ok)} ok, {len(missing)} missing, "
          f"{len(outdated)} outdated, {len(source_link_issues)} source-link issues")

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

    if source_link_issues:
        print("\nSource-link issues:")
        for spec, issue in source_link_issues:
            print(f"  {spec} — {issue}")

    if missing or outdated or source_link_issues:
        print()
        sys.exit(1)
    else:
        print("All specs up to date.")

if __name__ == "__main__":
    main()
