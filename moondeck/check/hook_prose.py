#!/usr/bin/env -S uv run --script
"""Claude Code PostToolUse hook: refuse a write that adds an em-dash or a British spelling.

The rules themselves live in the coding standards and are checked by check_prose.py at the
commit gate. This hook moves that check to the MOMENT OF WRITING, which is the only place it
can actually change behavior: an author does not notice these in their own prose, so finding
out 200 lines later means a sweep, while finding out on the edit means a rewrite of the one
sentence still in mind.

Wired in .claude/settings.json as a PostToolUse hook on Write|Edit. Exit 2 tells Claude Code
the tool call had a problem and feeds stderr back, so the fix happens in context.

Only fires on the suffixes the standards govern (check_prose.py owns that list), and only on
ADDED lines, so pre-existing prose in a file being edited is never the writer's problem.

TEMPORARY, together with check_prose.py: both go once the tree is swept clean and the
Vale workflow (.github/workflows/prose.yml) checks whole files.
"""

import json
import shutil
import subprocess
import sys
from pathlib import Path

CHECK = Path(__file__).with_name("check_prose.py")


def main() -> int:
    # The hook payload arrives as JSON on stdin. A malformed or absent payload must not block
    # the write: this check is a guardrail, not a gatekeeper on the tool protocol itself.
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0

    path = (payload.get("tool_input") or {}).get("file_path", "")
    if not path:
        return 0
    # check_prose.py governs which suffixes carry prose; mirror its list rather than a second
    # opinion that could drift from it.
    sys.path.insert(0, str(CHECK.parent))
    try:
        import check_prose  # noqa: PLC0415  (deliberate: the suffix list has ONE home)
    except ImportError:
        return 0
    if not path.endswith(check_prose.SUFFIXES):
        return 0

    repo = CHECK.parents[2]
    # Absolute tool paths and check=False on every call: a hook runs with whatever PATH the
    # editor had, and an implicit check would raise rather than let the write proceed.
    git = shutil.which("git")
    uv = shutil.which("uv")
    if not git or not uv:
        return 0
    # A brand-new file is invisible to `git diff` until git knows of it, and a new file is
    # exactly where fresh prose lands. `add -N` records the path without staging content, which
    # is enough for the diff to show its lines and leaves the index otherwise untouched.
    tracked = subprocess.run(
        [git, "ls-files", "--error-unmatch", "--", path],
        capture_output=True, text=True, cwd=repo, check=False,
    ).returncode == 0
    if not tracked:
        # `--` so a path that starts with a dash is a path, not a flag.
        subprocess.run([git, "add", "-N", "--", path], capture_output=True, cwd=repo, check=False)

    result = subprocess.run(
        [uv, "run", str(CHECK)], capture_output=True, text=True, cwd=repo, check=False
    )
    # Undo the intent-to-add. The index is the product owner's: a file they have not reviewed
    # must not appear staged in `git status`, where the "commit now covers only what was
    # reviewed" rule reads it.
    if not tracked:
        subprocess.run([git, "reset", "-q", "--", path], capture_output=True, cwd=repo,
                       check=False)
    if result.returncode == 0:
        return 0

    # Report only findings for the file just written. The check reports the whole diff, and a
    # finding in some other file is not this write's business (it will be caught on its own
    # write, or at the gate). Matched on the REPO-RELATIVE path, not the basename: two files
    # can share a name in different folders, and a basename match would report one against the
    # other. Detail lines are indented under their heading, so they are kept only while the
    # heading above them is ours.
    try:
        rel = str(Path(path).resolve().relative_to(repo))
    except ValueError:
        rel = path
    lines, ours = [], False
    for ln in result.stdout.splitlines():
        if ln.startswith("    "):
            if ours: lines.append(ln)
            continue
        ours = rel in ln
        if ours: lines.append(ln)
    if not lines:
        return 0

    print(
        "Prose rule (CLAUDE.md, coding-standards): American spelling, no em-dashes.\n"
        + "\n".join(lines)
        + "\n\nFix the line just written: a comma, colon or full stop where the em-dash is, "
          "and the US spelling (color, serialize, behavior, analyze).",
        file=sys.stderr,
    )
    return 2   # tells Claude Code to surface stderr back to the model


if __name__ == "__main__":
    sys.exit(main())
