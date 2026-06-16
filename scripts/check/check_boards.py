#!/usr/bin/env python3
"""Validate the installer board catalog (docs/install/boards.json).

The catalog is hand-maintained data consumed identically by three clients (the
web installer, the device UI's ?board= inject, and MoonDeck), so a typo drifts
silently — a broken image path, a board name that no longer matches its
Board.board control, a driver pin list on a board that has no driver. This is the
catalog's counterpart to check_specs.py for module docs: a fast, dependency-free
gate that pins the invariants the clients assume.

Invariants checked per entry:
  - required fields present (name, chip, firmwares, modules)
  - firmwares is a non-empty list of non-empty strings (entry[0] is the default)
  - image (if set) is a local assets/boards/ path that resolves on disk
  - url (if set) is an absolute http(s) link
  - the Board module's `board` control value equals the entry `name`
  - every module `type` is factory-registered (or a known boot-wired singleton)
  - a driver's `pins` control only appears on an actual *LedDriver module
  - supported/planned (if set) are string arrays drawn from the known vocabulary

Exit 1 on any error, mirroring check_specs.py.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
CATALOG = ROOT / "docs" / "install" / "boards.json"
MAIN_CPP = ROOT / "src" / "main.cpp"
DOCS = ROOT / "docs"

# Capability vocabulary. supported = what a module drives today; keep this list
# in lockstep with the modules that actually exist. planned = peripherals with no
# module yet (the backlog seed) — open-ended by design, so it is NOT whitelisted,
# only type-checked. Adding a new supported capability means a module backs it.
SUPPORTED_VOCAB = {"LEDs", "WiFi", "Ethernet", "Audio"}

# Boot-wired singletons: present on every device, added by code, so the catalog
# references them by id without the factory creating them. Their catalog `type`
# is the short id, not the factory class name (e.g. "Board", not "BoardModule").
BOOT_WIRED_TYPES = {"Board", "System", "Network", "Drivers"}


def registered_types():
    """The set of factory type names from main.cpp's registerType<T>("Name") calls."""
    text = MAIN_CPP.read_text(encoding="utf-8")
    return set(re.findall(r'registerType<[^>]+>\("([^"]+)"', text))


def main():
    errors = []

    try:
        catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as e:
        print(f"Board check: cannot read {CATALOG.relative_to(ROOT)}: {e}")
        sys.exit(1)

    if not isinstance(catalog, list):
        print("Board check: boards.json must be a JSON array")
        sys.exit(1)

    factory_types = registered_types()
    valid_types = factory_types | BOOT_WIRED_TYPES
    names_seen = set()

    for i, e in enumerate(catalog):
        where = f"entry {i}"
        if isinstance(e, dict) and "name" in e:
            where = f'"{e["name"]}"'

        if not isinstance(e, dict):
            errors.append(f"{where}: not a JSON object")
            continue

        # --- required fields ---
        for field in ("name", "chip", "firmwares", "modules"):
            if field not in e:
                errors.append(f"{where}: missing required field '{field}'")

        name = e.get("name")
        if name in names_seen:
            errors.append(f"{where}: duplicate board name")
        names_seen.add(name)

        # --- firmwares (firmwares[0] is the default the picker pre-selects) ---
        fws = e.get("firmwares")
        if not isinstance(fws, list):
            errors.append(f"{where}: firmwares must be a list, got {type(fws).__name__}")
        elif not fws:
            errors.append(f"{where}: firmwares must be a non-empty list (entry[0] is the default)")
        else:
            non_str = [f for f in fws if not isinstance(f, str)]
            if non_str:
                errors.append(f"{where}: firmwares entries must be strings, got "
                              f"{[type(f).__name__ for f in non_str]}")
            elif any(not f for f in fws):
                errors.append(f"{where}: firmwares entries must be non-empty strings")
            elif any(f != f.strip() or not f.strip() for f in fws):
                # Whitespace-only or padded keys (" ", "esp32 ") silently miss the
                # exact firmware-key match downstream (picker, dedup, manifest names).
                errors.append(f"{where}: firmwares entries must not be whitespace-only "
                              f"or have leading/trailing whitespace")

        # --- image resolves on disk + is a local path ---
        img = e.get("image")
        if img is not None:
            if not isinstance(img, str) or not img.startswith("assets/boards/"):
                errors.append(f"{where}: image must be a local 'assets/boards/...' path, got {img!r}")
            elif not (DOCS / img).exists():
                errors.append(f"{where}: image '{img}' does not exist on disk")

        # --- url is an absolute http(s) link ---
        url = e.get("url")
        if url is not None and not (isinstance(url, str) and url.startswith(("http://", "https://"))):
            errors.append(f"{where}: url must be an absolute http(s) link, got {url!r}")

        # --- capability fields ---
        for cap_field, whitelist in (("supported", SUPPORTED_VOCAB), ("planned", None)):
            caps = e.get(cap_field)
            if caps is None:
                continue
            if not isinstance(caps, list) or not all(isinstance(c, str) for c in caps):
                errors.append(f"{where}: {cap_field} must be a list of strings")
                continue
            if whitelist is not None:
                for c in caps:
                    if c not in whitelist:
                        errors.append(f"{where}: supported capability '{c}' is not in the "
                                      f"known vocabulary {sorted(whitelist)} — add a module first")

        # --- modules ---
        mods = e.get("modules")
        if not isinstance(mods, list):
            # `modules` is required (presence checked above); a wrong *type* is a
            # schema violation, not something to skip silently.
            errors.append(f"{where}: modules must be a list, got {type(mods).__name__}")
            continue
        board_control_seen = False
        for m in mods:
            if not isinstance(m, dict):
                errors.append(f"{where}: a modules entry is not an object")
                continue
            mtype = m.get("type")
            mid = m.get("id")
            if not mtype:
                errors.append(f"{where}: a modules entry has no 'type'")
            elif mtype not in valid_types:
                errors.append(f"{where}: module type '{mtype}' is not factory-registered "
                              f"(and not a boot-wired singleton)")
            if not isinstance(mid, str) or not mid:
                errors.append(f"{where}: module '{mtype}' has no non-empty 'id'")

            controls = m.get("controls") or {}
            # Board.board control must equal the entry name (the device's identity key).
            if mtype == "Board" and isinstance(controls, dict) and "board" in controls:
                board_control_seen = True
                if controls["board"] != name:
                    errors.append(f"{where}: Board.board control '{controls['board']}' "
                                  f"!= entry name '{name}'")
            # A `pins` control only makes sense on an LED driver module.
            if isinstance(controls, dict) and "pins" in controls and not str(mtype).endswith("LedDriver"):
                errors.append(f"{where}: module '{mtype}' has a 'pins' control but is not a *LedDriver")

        if mods and not board_control_seen:
            errors.append(f"{where}: no Board module sets the 'board' identity control")

    # Report (mirrors check_specs.py's shape).
    print(f"Board check: {len(catalog)} boards, {len(errors)} issue(s)")
    if errors:
        print()
        for err in errors:
            print(f"  {err}")
        print()
        sys.exit(1)
    print("Catalog valid.")


if __name__ == "__main__":
    main()
