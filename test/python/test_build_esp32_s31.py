"""build_esp32.py contracts for the ESP32-S31 (a RISC-V preview target).

The S31 needed three pieces of build wiring, each with a sharp failure mode if it
drifts. This pins them so a refactor of the FIRMWARES dict can't silently break the
S31 build or its CI matrix entry:

  1. FIRMWARES["esp32s31"] is well-formed and every chip has a family label, so the
     manifest generator (CHIP_FAMILIES) can't KeyError on it.
  2. esp32s31 is in PREVIEW_TARGETS — without it `idf.py set-target esp32s31` refuses
     ("you have to append '--preview'"), so the build never starts.
  3. The release workflow infers the IDF target from the firmware-name PREFIX. Because
     "esp32s31" also startsWith "esp32s3", the esp32s31 check MUST come first, or CI
     would build the S31 firmware for the WRONG target (esp32s3). We re-implement the
     same precedence rule here and pin that esp32s31 resolves to esp32s31.

Imports the real dicts from scripts/build/build_esp32.py (no ESP-IDF needed).
Run: `uv run --with pytest pytest test/python -q`.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "build"))

from build_esp32 import FIRMWARES, TARGET_TO_FAMILY, PREVIEW_TARGETS  # noqa: E402


def test_s31_firmware_entry_is_well_formed():
    fw = FIRMWARES.get("esp32s31")
    assert fw is not None, "FIRMWARES must define esp32s31"
    assert fw["chip"] == "esp32s31"
    assert fw["eth_only"] is False, "the S31 build is all-in-one WiFi+Eth, not eth-only"
    assert fw["ships"] is True, "the S31 ships (appears in the web installer + CI matrix)"
    # The fragment file it layers must exist on disk, or the build fails at configure.
    frag = "sdkconfig.defaults.esp32s31"
    assert frag in fw["fragments"], "the S31 entry must layer its own sdkconfig fragment"
    assert (ROOT / "esp32" / frag).is_file(), f"{frag} is referenced but missing on disk"


def test_every_firmware_chip_has_a_family_label():
    # generate_manifest.py builds CHIP_FAMILIES = {fw: TARGET_TO_FAMILY[chip]} — a chip
    # without a family entry would KeyError there. Pin that none is missing (S31 included).
    for name, spec in FIRMWARES.items():
        assert spec["chip"] in TARGET_TO_FAMILY, (
            f'firmware "{name}" has chip "{spec["chip"]}" with no TARGET_TO_FAMILY entry — '
            f"generate_manifest.py would KeyError. Add it to TARGET_TO_FAMILY."
        )
    assert TARGET_TO_FAMILY["esp32s31"] == "ESP32-S31"


def test_s31_is_a_preview_target():
    # idf.py set-target esp32s31 refuses without --preview; build_esp32.py adds the flag
    # only for chips in PREVIEW_TARGETS. Drop esp32s31 from this set once it graduates.
    assert "esp32s31" in PREVIEW_TARGETS


def test_ci_target_inference_resolves_esp32s31_before_esp32s3():
    # Re-implement the release.yml `target:` precedence: esp32s31 → esp32s3 → esp32p4 →
    # esp32. The esp32s31 check is FIRST on purpose — "esp32s31".startswith("esp32s3") is
    # True, so a wrong order would map the S31 firmware to the esp32s3 IDF target.
    def infer_target(firmware: str) -> str:
        if firmware.startswith("esp32s31"):
            return "esp32s31"
        if firmware.startswith("esp32s3"):
            return "esp32s3"
        if firmware.startswith("esp32p4"):
            return "esp32p4"
        return "esp32"

    assert infer_target("esp32s31") == "esp32s31", "S31 must NOT be misread as esp32s3"
    assert infer_target("esp32s3-n16r8") == "esp32s3"
    assert infer_target("esp32s3-n8r8") == "esp32s3"
    assert infer_target("esp32p4-eth") == "esp32p4"
    assert infer_target("esp32") == "esp32"
    assert infer_target("esp32-16mb") == "esp32"

    # Belt-and-suspenders: the rule must agree with each real firmware's declared chip.
    for name, spec in FIRMWARES.items():
        assert infer_target(name) == spec["chip"], (
            f'the CI target-inference rule maps "{name}" to "{infer_target(name)}" but its '
            f'declared chip is "{spec["chip"]}" — release.yml would set the wrong target.'
        )
