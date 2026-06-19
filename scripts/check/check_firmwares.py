#!/usr/bin/env python3
"""Check that docs/install/firmwares.json is in sync with the FIRMWARES dict.

firmwares.json is a generated projection of build_esp32.py's FIRMWARES (the single
source of truth), read by the CI release matrix, the ESP Web Tools manifest loops,
and MoonDeck. This guard regenerates the projection in-memory and fails if the
committed file drifted — so editing FIRMWARES without regenerating is caught at
commit time, not at release. The catalog counterpart to check_devices.py.

Exit 1 on drift (with the regenerate command), mirroring check_devices.py.
"""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
COMMITTED = ROOT / "docs" / "install" / "firmwares.json"

# Reuse the generator's projection so the checker and generator can't disagree.
sys.path.insert(0, str(ROOT / "scripts" / "build"))
from generate_firmwares import build_doc  # noqa: E402


def main():
    expected = build_doc()
    n = len(expected["firmwares"])

    try:
        actual = json.loads(COMMITTED.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        print(f"Firmware check: cannot read {COMMITTED.relative_to(ROOT)}: {e}")
        sys.exit(1)

    # Compare parsed dicts, not bytes — formatting (ensure_ascii, trailing
    # newline) can't cause spurious drift; only the data matters.
    if actual != expected:
        print(f"Firmware check: {n} variants, DRIFT")
        print("  docs/install/firmwares.json is stale — regenerate with:")
        print("  uv run scripts/build/generate_firmwares.py --out docs/install/firmwares.json")
        sys.exit(1)

    print(f"Firmware check: {n} variants, 0 issue(s)")
    print("Firmware list valid.")


if __name__ == "__main__":
    main()
