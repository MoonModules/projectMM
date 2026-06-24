#!/usr/bin/env python3
"""Compute the semver string a build should carry, per build type.

`library.json` holds the in-development version with a `-dev` prerelease suffix
(e.g. `2.1.0-dev`). That's right for a local build, but a *published* build needs
a precise, comparable version:

  - Stable release (a `vX.Y.Z` tag): the CORE semver, suffix dropped — `2.1.0`.
    (The release ritual already bumps library.json to the core before tagging;
    this just normalises in case the suffix lingers.)
  - Moving `latest` build (every push to main): a MONOTONIC prerelease —
    `<core>-dev.<N>` where N = commits since the last stable `vX.Y.Z` tag
    (git-describe style). Successive `latest` builds get `…-dev.6`, `…-dev.7`, …
    which semver §11 orders numerically, so a device can tell it's behind the
    newest `latest`. Without N, every `latest` build claims the same version.

N counts since the last `v*` tag specifically — NOT `git describe`'s nearest tag,
which would pick the moving `latest` tag and give a meaningless count. With no
`v*` tag yet, N falls back to the total commit count from the root.

The same string is burned into the binary (-DMM_VERSION), the release asset names
(firmware-<F>-v<V>.bin), and the ESP Web Tools manifest, so all three agree.

Usage:
  compute_version.py --channel stable          # → core, e.g. 2.1.0
  compute_version.py --channel latest          # → <core>-dev.<N>, e.g. 2.1.0-dev.6
  compute_version.py                           # defaults to library.json verbatim (local)
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
LIBRARY_JSON = ROOT / "library.json"


def core_version(version: str) -> str:
    """Strip any -prerelease / +build suffix → the MAJOR.MINOR.PATCH core."""
    return version.split("-")[0].split("+")[0]


def commits_since_last_stable() -> int:
    """Commits since the last `vX.Y.Z` tag (not the moving `latest` tag).

    Falls back to the total commit count when no `v*` tag exists yet.
    """
    try:
        last = subprocess.run(
            ["git", "describe", "--tags", "--abbrev=0", "--match", "v*"],
            cwd=ROOT, capture_output=True, text=True, check=True,
        ).stdout.strip()
        rng = f"{last}..HEAD"
    except subprocess.CalledProcessError:
        rng = "HEAD"  # no v* tag yet — count from the root
    out = subprocess.run(
        ["git", "rev-list", "--count", rng],
        cwd=ROOT, capture_output=True, text=True, check=True,
    ).stdout.strip()
    return int(out)


def compute(channel: str) -> str:
    version = json.loads(LIBRARY_JSON.read_text(encoding="utf-8"))["version"]
    core = core_version(version)
    if channel == "stable":
        return core
    if channel == "latest":
        return f"{core}-dev.{commits_since_last_stable()}"
    return version  # local: library.json verbatim


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--channel", choices=["stable", "latest", "local"], default="local",
        help="stable → core semver; latest → <core>-dev.<N>; local → library.json verbatim",
    )
    args = parser.parse_args()
    print(compute(args.channel))
    return 0


if __name__ == "__main__":
    sys.exit(main())
