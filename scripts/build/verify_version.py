#!/usr/bin/env python3
"""Verify that the git tag matches `library.json["version"]`.

Runs as the first job in release.yml. Fails the workflow when the tag and
the in-tree version disagree, so a release never publishes binaries whose
filenames lie about the version they ship.

Tag → version mapping: strip a leading 'v'. Both `v1.0.0` and `1.0.0` are
accepted as the tag, but the in-tree version is the bare semver.

Inputs:
  GITHUB_REF_NAME  — the tag, set by GitHub Actions on a `push: tags` event
                     (or the workflow_dispatch input forwarded to it).
  --tag <name>     — explicit override, useful for local checks.
"""

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
LIBRARY_JSON = ROOT / "library.json"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", help="Tag to verify (defaults to $GITHUB_REF_NAME)")
    args = parser.parse_args()

    import os
    tag = args.tag or os.environ.get("GITHUB_REF_NAME")
    if not tag:
        print("verify_version: no tag supplied (--tag or $GITHUB_REF_NAME).")
        return 2

    tag_version = tag[1:] if tag.startswith("v") else tag

    try:
        meta = json.loads(LIBRARY_JSON.read_text())
    except FileNotFoundError:
        print(f"verify_version: {LIBRARY_JSON} not found.")
        return 2
    except json.JSONDecodeError as e:
        print(f"verify_version: {LIBRARY_JSON} is not valid JSON: {e}")
        return 2

    in_tree = meta.get("version")
    if not in_tree:
        print(f"verify_version: 'version' missing from {LIBRARY_JSON}.")
        return 2

    if tag_version != in_tree:
        print(
            f"verify_version: tag '{tag}' (version {tag_version}) "
            f"does not match library.json version '{in_tree}'.\n"
            f"Bump library.json to {tag_version}, commit, retag, push."
        )
        return 1

    print(f"verify_version: OK (tag {tag} ↔ library.json {in_tree}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
