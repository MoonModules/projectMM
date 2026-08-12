"""Contracts for the JC-ESP32P4-M3-DEV firmware and device model."""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "build"))

from build_esp32 import FIRMWARES, firmware_cmake_args  # noqa: E402


def test_jc_m3_firmware_selects_the_board_fragment():
    firmware = FIRMWARES.get("esp32p4-jc-m3-eth")
    assert firmware is not None
    assert firmware["chip"] == "esp32p4"
    assert firmware["eth_only"] is True
    assert firmware["ships"] is True

    fragment = "sdkconfig.defaults.esp32p4-jc-m3"
    assert fragment in firmware["fragments"]
    assert (ROOT / "esp32" / fragment).read_text(encoding="utf-8").strip().endswith(
        "CONFIG_MM_JC_P4_M3=y"
    )
    assert fragment in firmware_cmake_args("esp32p4-jc-m3-eth")[0]


def test_jc_m3_codec_dependency_is_kconfig_selected():
    manifest = (ROOT / "esp32" / "main" / "idf_component.yml").read_text(encoding="utf-8")
    assert "matches:" in manifest
    assert '$CONFIG{MM_JC_P4_M3} == True' in manifest

    platform_config = (
        ROOT / "src" / "platform" / "esp32" / "platform_config.h"
    ).read_text(encoding="utf-8")
    assert "defined(CONFIG_MM_JC_P4_M3)" in platform_config
    assert "{ /*sda*/ 7, /*scl*/ 8, /*mclk*/ 13, /*addr*/ 0x18 }" in platform_config

    codec_source = (
        ROOT / "src" / "platform" / "esp32" / "platform_esp32_es8311.cpp"
    ).read_text(encoding="utf-8")
    assert "i2cCtrlCfg.addr = pins.i2cAddr << 1;" in codec_source
    assert "es8311Cfg.digital_mic = false;" in codec_source
    assert "fs.bits_per_sample = 32;" in codec_source
    assert "st->codec->set_fs(st->codec, &fs)" in codec_source
    assert "st->codec->enable(st->codec, true)" in codec_source
    assert "esp_codec_dev_new(" not in codec_source


def test_jc_m3_device_model_applies_the_schematic_audio_pins():
    models = json.loads(
        (ROOT / "web-installer" / "deviceModels.json").read_text(encoding="utf-8")
    )
    model = next(entry for entry in models if entry["name"] == "JC-ESP32P4-M3-DEV")
    assert model["firmwares"] == ["esp32p4-jc-m3-eth"]
    assert "Audio" in model["supported"]

    audio = next(module for module in model["modules"] if module["type"] == "AudioService")
    assert audio["parent_id"] == "Services"
    assert audio["controls"] == {"sckPin": 12, "wsPin": 10, "sdPin": 48}

    scanner = next(
        module for module in model["modules"] if module["type"] == "I2cScanModule"
    )
    assert scanner["controls"] == {"sda": 7, "scl": 8}
