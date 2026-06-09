#!/usr/bin/env python3
"""Generate an ESP Web Tools manifest for one firmware variant.

ESP Web Tools (https://esphome.github.io/esp-web-tools/) flashes ESP32
firmware from the browser using Web Serial. It reads a JSON manifest that
lists which `.bin` parts to write at which offsets, keyed by chip family.

Schema reference: https://esphome.github.io/esp-web-tools/#manifest

The release pipeline calls this once per firmware variant. Each variant gets
its own manifest because:
  * the firmware bundle differs per variant (sdkconfig fragments → different
    bootloader and partition table),
  * the chip family differs (ESP32 vs ESP32-S3), and so does the bootloader
    offset on flash (0x1000 vs 0x0 — wrong offset bricks visibly).

"Firmware" here is the compiled binary variant — separate from "board" (the
physical hardware). See docs/architecture.md § Firmware vs board.

We don't hardcode the offsets. ESP-IDF writes them into
`build/flasher_args.json` for the exact chip it just built. The CI stage
copies that file alongside the bins, and we parse it here.

Inputs:
  --firmware <key>        — firmware variant key (matches the firmware
                            filename prefix).
  --version <ver>         — release version, e.g. "1.0.0".
  --release-url <url>     — base URL to the GitHub release assets.
  --flasher-args <path>   — esp32/build/flasher_args.json from the build.
  --out <path>            — manifest JSON destination.
"""

import argparse
import json
import sys
from pathlib import Path

# ESP-IDF writes a number of `<name>.bin` references in flasher_args.json,
# each keyed by hex offset. We map IDF's output names to the firmware-bundle
# filenames the release publishes (the same names release.yml stages from
# esp32/build/).
PART_NAME_MAP = {
    "projectMM.bin": "{prefix}.bin",
    "bootloader.bin": "{prefix}-bootloader.bin",
    "partition-table.bin": "{prefix}-partition-table.bin",
    "ota_data_initial.bin": "{prefix}-ota-data.bin",
}

# firmware → chip family string ESP Web Tools accepts.
# Full list: https://github.com/espressif/esptool-js/blob/main/src/esploader.ts
CHIP_FAMILIES = {
    "esp32":           "ESP32",
    "esp32-eth":       "ESP32",
    "esp32-eth-wifi":  "ESP32",
    "esp32s3-n16r8":   "ESP32-S3",
    "esp32s3-n8r8":    "ESP32-S3",
}


def parts_from_flasher_args(flasher_args: dict, prefix: str) -> list[dict]:
    """Convert IDF's flasher_args.json `flash_files` map into Web Tools parts."""
    flash_files = flasher_args.get("flash_files", {})
    parts: list[dict] = []
    for offset_hex, bin_name in flash_files.items():
        # Some entries are full paths (e.g. "bootloader/bootloader.bin"); we
        # match on basename so the lookup table stays firmware-neutral.
        basename = Path(bin_name).name
        template = PART_NAME_MAP.get(basename)
        if not template:
            print(f"generate_manifest: skipping unknown part '{bin_name}'")
            continue
        offset = int(offset_hex, 16) if isinstance(offset_hex, str) else int(offset_hex)
        parts.append({
            "path": template.format(prefix=prefix),
            "offset": offset,
        })
    # Web Tools doesn't require a specific order, but sorted-by-offset is the
    # human-readable convention every other project uses.
    parts.sort(key=lambda p: p["offset"])
    return parts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", required=True, choices=sorted(CHIP_FAMILIES))
    parser.add_argument("--version", required=True)
    parser.add_argument("--release-url", required=True,
                        help="Base URL where the .bin assets live (no trailing slash).")
    parser.add_argument("--flasher-args", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    try:
        flasher_args = json.loads(args.flasher_args.read_text())
    except FileNotFoundError:
        print(f"generate_manifest: {args.flasher_args} not found")
        return 2
    except json.JSONDecodeError as e:
        print(f"generate_manifest: {args.flasher_args} not valid JSON: {e}")
        return 2

    # Firmware filenames in the release: firmware-<firmware>-v<version>{,-bootloader,-partition-table,-ota-data}.bin
    prefix = f"firmware-{args.firmware}-v{args.version}"
    base_url = args.release_url.rstrip("/")

    parts = parts_from_flasher_args(flasher_args, prefix)
    if not parts:
        print(f"generate_manifest: no recognised parts in {args.flasher_args}")
        return 1

    # ESP Web Tools resolves the per-part `path` relative to the manifest URL,
    # so absolute URLs are the simplest robust shape — the manifest stays
    # correct whether it's served from Pages, a release page, or a mirror.
    for p in parts:
        p["path"] = f"{base_url}/{p['path']}"

    manifest = {
        "name": "projectMM",
        "version": args.version,
        "home_assistant_domain": "projectMM",
        # Wipe-first on flash. Right default while config schemas are
        # unstable (avoids stale-state bugs across versions). Revisit when
        # the schema is locked.
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": CHIP_FAMILIES[args.firmware],
                "parts": parts,
            }
        ],
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"generate_manifest: wrote {args.out} ({len(parts)} parts, "
          f"{CHIP_FAMILIES[args.firmware]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
