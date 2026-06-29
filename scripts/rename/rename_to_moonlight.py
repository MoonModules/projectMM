#!/usr/bin/env python3
"""Mechanical projectMM -> MoonLight rename sweep (dry-run by default).

The one-shot transition tool for [docs/backlog/rename-to-moonlight.md] Phase 1
step 4. Replaces the product-name token everywhere it is the *current* name,
while leaving the references that must NOT change: the predecessor "MoonLight"
prose/links (already repointed to ewowi/MoonLight), the MoonLive scripting
engine (a different name), and the docs/history era record (which keeps saying
projectMM by the present-tense history exception).

Run it NOW against today's tree to harden the exclude list and review the diff,
but DRY-RUN ONLY until the switch — the externally-visible identifiers (binary
name, mDNS prefix, repo URL, library.json name) must flip *at* the switch, lined
up with the first release under the new repo (see the plan's Phase 2/3).

ORDERING: the sweep also rewrites the repo URL (MoonModules/projectMM ->
MoonModules/MoonLight) and the docs host (moonmodules.org/projectMM). Per the
plan that is correct ONLY when run *after* the repo rename (Phase 3.2): the URL
becomes MoonModules/MoonLight, which only resolves once this repo holds that
name. So --apply belongs in Phase 3.3, after 3.2 — not before.

  uv run scripts/rename/rename_to_moonlight.py            # dry-run: list every hit, change nothing
  uv run scripts/rename/rename_to_moonlight.py --apply    # WRITE the changes (switch-day only)

Why a script and not a bare `sed`: the value here is not clever per-form logic
(a plain token replace is correct for every form — repo URL, host path,
binary basename, product name, deviceName slug all just contain the token), it
is the EXCLUDE LIST + the dry-run review + the binary/.git guards. Verified
against today's tree: `projectMM` is never a substring of another token, and the
rename touches neither `MoonLight` nor `MoonLive`, so those are safe by
construction — the only deliberate handling is the capitalised enum token
`ProjectMM` and the path exclusions below.

Exit 1 if a dry-run finds hits (so CI / a pre-switch check can assert "still
references to flip"); exit 0 when clean (post-switch, nothing left).
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# Ordered token replacements. `ProjectMM` (the DevType enum) before `projectMM`
# so the capitalised form is handled as its own token, not left half-rewritten.
# Both map to the same product name; the longer forms (MoonModules/projectMM,
# projectMM.bin, moonmodules.org/projectMM) fall out of replacing the token
# inside them — no per-form rules needed.
REPLACEMENTS = [
    ("ProjectMM", "MoonLight"),   # DevType::ProjectMM enum. Safe: classification keys
                                  #   on the "modules" marker (DeviceIdentify.h), not this
                                  #   token — devTypeStr's "projectMM" is only a UI label.
    ("projectMM", "MoonLight"),   # the product name in every other form
]

# The file list comes from `git ls-files` (tracked files only), so build output
# (build/, esp32/build/ — both gitignored) and other artifacts never enter the
# sweep, without maintaining a brittle path blocklist. Only these directory
# prefixes are then *additionally* excluded for content reasons:
EXCLUDE_DIRS = [
    "docs/history",              # records the projectMM ERA by name — present-tense
                                  #   history exception (CLAUDE.md § Principles)
]
EXCLUDE_FILES = [
    # This plan describes BOTH names and the move between them; rewriting it would
    # corrupt its meaning ("the predecessor at MoonModules/MoonLight vacates…").
    "docs/backlog/rename-to-moonlight.md",
    # The rename script itself (it names the tokens it replaces).
    "scripts/rename/rename_to_moonlight.py",
]

# File extensions the sweep considers (text formats that carry the name). Binary
# assets (.png/.jpg/.bin) are skipped both by extension and by the text-read
# guard below.
INCLUDE_SUFFIXES = {
    ".md", ".py", ".h", ".cpp", ".js", ".json", ".html", ".css",
    ".txt", ".csv", ".cmake", ".yml", ".yaml", ".ini",
}
# Files with no suffix that still matter (e.g. CMakeLists.txt handled via suffix;
# add bare names here if needed). CMakeLists.txt has a .txt-like role but no
# suffix match, so include it explicitly by name.
INCLUDE_NAMES = {"CMakeLists.txt"}


def is_excluded(rel: str) -> bool:
    for d in EXCLUDE_DIRS:
        if rel == d or rel.startswith(d + "/"):
            return True
    return rel in EXCLUDE_FILES


def wanted(path: Path) -> bool:
    if path.suffix in INCLUDE_SUFFIXES:
        return True
    return path.name in INCLUDE_NAMES


def iter_files():
    # Tracked files only — `git ls-files` respects .gitignore, so build output
    # (build/, esp32/build/) is never swept and we don't maintain a blocklist.
    # Caveat: gitignored files are therefore NOT covered — notably the private
    # bench registry scripts/moondeck.json, whose "board" names must be
    # hand-updated at switch-time (see the plan's Phase 1 step 5).
    out = subprocess.run(
        ["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout
    for rel in sorted(out.splitlines()):
        if not rel or is_excluded(rel):
            continue
        path = ROOT / rel
        if not path.is_file() or not wanted(path):
            continue
        yield path


def count_hits(text: str) -> int:
    return sum(text.count(old) for old, _ in REPLACEMENTS)


def apply_replacements(text: str) -> str:
    for old, new in REPLACEMENTS:
        text = text.replace(old, new)
    return text


def main() -> int:
    ap = argparse.ArgumentParser(description="projectMM -> MoonLight rename sweep")
    ap.add_argument("--apply", action="store_true",
                    help="WRITE the changes (default is a dry-run that changes nothing)")
    args = ap.parse_args()

    total_hits = 0
    touched = 0
    for path in iter_files():
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, ValueError):
            continue  # binary / non-utf8 — the text guard, belt-and-braces with the suffix filter
        hits = count_hits(text)
        if hits == 0:
            continue
        total_hits += hits
        touched += 1
        rel = path.relative_to(ROOT).as_posix()
        if args.apply:
            path.write_text(apply_replacements(text), encoding="utf-8")
            print(f"  rewrote {rel} ({hits} hit{'s' if hits != 1 else ''})")
        else:
            print(f"  {rel}: {hits} hit{'s' if hits != 1 else ''}")
            # show each line so the dry-run is reviewable
            for n, line in enumerate(text.splitlines(), 1):
                if any(old in line for old, _ in REPLACEMENTS):
                    print(f"      {n}: {line.strip()[:100]}")

    mode = "APPLIED" if args.apply else "DRY-RUN"
    print(f"\n[{mode}] {total_hits} hit(s) across {touched} file(s).")
    if args.apply:
        print("Changes written. Run the full gate set (build all ESP32 variants, "
              "ctest, scenarios, check_devices, check_specs) before committing.")
        return 0
    # Dry-run: non-zero when there is still something to flip, so a pre-switch
    # check can assert the work isn't done; zero post-switch when clean.
    return 1 if total_hits else 0


if __name__ == "__main__":
    sys.exit(main())
