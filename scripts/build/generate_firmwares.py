#!/usr/bin/env python3
"""Generate docs/install/firmwares.json from the FIRMWARES dict.

The firmware-variant list was hand-copied across the CI matrix, the two ESP Web
Tools manifest loops, MoonDeck, and the docs — six copies that drifted (MoonDeck's
even dropped the P4 variants). This script projects the single source of truth —
the `FIRMWARES` dict in build_esp32.py — into one machine-readable file every
consumer reads instead:

  * release.yml — the build matrix + both manifest loops select `.ships`,
  * MoonDeck — its firmware picker,
  * docs/building.md — points here instead of restating the list.

check_firmwares.py guards the committed file against drift from FIRMWARES.

"Firmware" here is the compiled binary variant — separate from "board" (the
physical hardware). See docs/architecture.md § Firmware vs board.

Inputs:
  --out <path>   — firmwares.json destination (docs/install/firmwares.json).
"""

import argparse
import json
import sys
from pathlib import Path

# build_esp32.py is the single source of truth for the firmware variants; import
# it so this script never holds a second hand-maintained list (same cross-script
# import generate_manifest.py and collect_kpi.py already use).
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_esp32 import FIRMWARES  # noqa: E402


def build_doc() -> dict:
    """Project FIRMWARES into the firmwares.json document.

    `fragments` is build-internal (sdkconfig merge order) and intentionally
    omitted — consumers care about name/chip/eth_only/ships/description.
    check_firmwares.py imports this so the generator and checker can't disagree
    on the projection.
    """
    return {
        "firmwares": [
            {
                "name": name,
                "chip": spec["chip"],
                "eth_only": spec["eth_only"],
                "ships": spec["ships"],
                "description": spec["description"],
            }
            for name, spec in FIRMWARES.items()
        ]
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", required=True, type=Path,
                        help="firmwares.json destination")
    args = parser.parse_args()

    args.out.parent.mkdir(parents=True, exist_ok=True)
    # ensure_ascii=False keeps the em-dashes in descriptions literal (readable
    # diffs), matching the hand-authored boards.json sibling.
    args.out.write_text(json.dumps(build_doc(), indent=2, ensure_ascii=False) + "\n")
    print(f"generate_firmwares: wrote {args.out} ({len(FIRMWARES)} variants)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
