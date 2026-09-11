#!/usr/bin/env -S uv run --script
"""Prose rules the coding standards state and nothing enforced: no em-dashes, US spelling.

Both rules were written down and then broken repeatedly, in the same commits that swept them
out of other files, because they are habits rather than decisions. A habit is not fixed by
intending to do better; it is fixed by a check that fails.

ADDED LINES ONLY. Pre-existing prose is not this check's business: rewriting a sentence a
change merely touched is churn that buries the actual diff, and the standards apply to new
prose. A line that only moved, or whose only edit was a rename, keeps whatever it had.

ADDED LINES ONLY IS THE WHOLE DESIGN, not a simplification to remove later. The tree holds
~7,700 pre-existing violating lines across ~490 files, two thirds of them in C++ and Python
comments, so a full-tree gate would fail every commit until a sweep whose blast radius is far
larger than any one change. Checking only what a change ADDS converges on the same end state
without that: every file converts as it is touched, and no commit pays for prose it did not
write. Registered in the commit gate on that basis, and wired as a write-time hook
(hook_prose.py) so the fix happens while the sentence is still in mind.

    uv run moondeck/check/check_prose.py
"""

import re
import subprocess
import sys

# Files whose prose the standards govern. Not .json or .txt: generated or data.
# .mle/.mll/.mlm are MoonLive scripts: shipped, opened in the device's own editor, and read by
# every user who learns the language, so they are the most user-facing prose in the repo.
SUFFIXES = (".h", ".hpp", ".c", ".cpp", ".inc", ".md", ".py", ".js", ".css", ".html",
            ".mle", ".mll", ".mlm")

# Paths exempt, with the reason each earns it.
EXEMPT = (
    "docs/friend-repos/", # monthly digests OF OTHER PROJECTS, quoted from their sources
    "docs/work/past/",    # dated records: what was true at a moment, kept unrewritten
    "docs/work/future/",  # prior-project digests quoted from their sources
    "docs/metrics/",      # generated
    "docs/tests/",        # generated from test comments (fix the test, not the page)
    "docs/moonmodules/",  # partly generated technical pages
    "src/platform/desktop/vendor/",   # upstream single-header code (miniaudio): not our prose
    "src/ui/vendor/",                 # upstream browser code (Prism): not our prose either
    "moondeck/check/check_prose.py",  # the detector: its rule table spells the very patterns
    "docs/documentation-standards.md",  # the RULE: it must quote an em-dash and "analyse" to ban them
)

# The banned character, by CODEPOINT rather than as a literal. Written literally, a sweep that
# rewrites em-dashes in this repo edits the detector itself: one such pass turned this into a
# comma and the check then flagged every comma in the tree. The en-dash (U+2013) and the arrow
# (U+2192) are NOT banned, so the test is this one codepoint and nothing else.
EM_DASH = "\u2014"

# British to American. Substring matches, so a stem covers its inflections.
SPELLING = {
    "behaviour": "behavior", "colour": "color", "initialis": "initializ",
    "optimis": "optimiz", "recognis": "recogniz",
    # The stem includes the e on purpose: the plain noun ("analysis"/"analyses") is already
    # US spelling (coding-standards names it a keeper), so only the e-form verbs are flagged.
    "analys" + "e": "analyze", "analys" + "ing": "analyzing",
    "materialis": "materializ", "normalis": "normaliz", "serialis": "serializ",
    "cancelled": "canceled", "modelling": "modeling", "labelled": "labeled",
    "centre": "center", "licence": "license", "defence": "defense",
}


def added_lines(base):
    """Every line this branch or working tree ADDS, as (path, line text)."""
    diff = subprocess.run(
        ["git", "diff", base, "--unified=0"], capture_output=True, text=True
    ).stdout
    path, out = None, []
    for line in diff.split("\n"):
        if line.startswith("+++ b/"):
            path = line[6:]
        elif line.startswith("+") and not line.startswith("+++") and path:
            out.append((path, line[1:]))
    return out


def main():
    # Against main when on a branch, else the working tree: the check means "what am I adding".
    base = "main...HEAD" if len(sys.argv) < 2 else sys.argv[1]
    if subprocess.run(["git", "rev-parse", "--verify", "main"],
                      capture_output=True).returncode != 0:
        base = "HEAD"

    findings = []
    for path, text in added_lines(base) + added_lines("HEAD"):
        if not path.endswith(SUFFIXES) or path.startswith(EXEMPT):
            continue
        if EM_DASH in text:
            # Quote AROUND the offending character, not the head of the line. A long line
            # truncated at 96 characters hides it and shows an arrow or a hyphen instead, which
            # reads as a false positive and teaches the reader to distrust the check.
            at = text.index(EM_DASH)
            findings.append((path, "em-dash", text[max(0, at - 40):at + 40].strip()))
        low = text.lower()
        for brit, amer in SPELLING.items():
            if brit in low:
                findings.append((path, f"{brit} -> {amer}", text.strip()[:96]))
                break

    # The same line can arrive from both diffs; report each once.
    findings = sorted(set(findings))
    if not findings:
        print("Prose check: no em-dashes or British spellings in added lines.")
        return 0

    print(f"Prose check: {len(findings)} issue(s) in ADDED lines.\n")
    for path, what, text in findings:
        print(f"  {path}: {what}")
        print(f"    {text}")
    print("\nAn em-dash reads as a habit rather than a choice: use a colon for an explanation,")
    print("commas or parentheses for an aside, or a full stop for two independent clauses.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
