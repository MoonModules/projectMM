# /// script
# dependencies = ["pytest", "pyserial"]
# ///
"""Improv frame-contract tests (Python side).

Pins the wire format the device C++ (src/core/ImprovFrame.h), the installer JS
(docs/install/improv-frame.js), and this Python builder (scripts/build/improv_provision.py)
must all agree on byte-for-byte. The G1 golden vector below is the SAME one asserted
in test/js/improv-frame.test.mjs, so the JS and Python envelope builders can't drift;
it's hand-verified against the C++ sum-mod-256 checksum too.

pyserial is an inline dep only because improv_provision.py's `import serial` guard
sys.exit()s when it's missing — the frame functions themselves need nothing. Run:
`uv run pytest test/python` (uv honours the inline deps above).

APPLY_OP chunking is JS+device-C++ only (the Python provisioning script does WIFI_SETTINGS,
not config push), so that layer is pinned in the JS test; here we pin the shared envelope.
"""

import sys
from pathlib import Path

# improv_provision.py lives in scripts/build and imports a sibling (host_wifi).
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "build"))

import improv_provision as ip  # noqa: E402


def test_checksum_is_sum_mod_256():
    assert ip.checksum(b"abc") == sum(b"abc") & 0xFF
    assert ip.checksum(b"") == 0
    assert ip.checksum(bytes([0xFF, 0xFF])) == 0xFE  # wraps mod 256


def test_frame_layout():
    frame = ip.build_frame(0x03, bytes([0x01]))
    assert frame[0:6] == b"IMPROV", "magic"
    assert frame[6] == 0x01, "version"
    assert frame[7] == 0x03, "type"
    assert frame[8] == 1, "length"
    assert frame[9] == 0x01, "payload"
    assert len(frame) == 11, "9 header + 1 payload + 1 checksum"


def test_golden_vector_g1():
    # Shared with test/js G1. Hand-verified checksum 0xe3.
    frame = ip.build_frame(0x03, bytes([0x01]))
    assert frame.hex(" ") == "49 4d 50 52 4f 56 01 03 01 01 e3"


def test_checksum_covers_header_through_payload():
    payload = bytes([0xAA, 0xBB, 0xCC])
    frame = ip.build_frame(0x03, payload)
    assert frame[-1] == sum(frame[:-1]) & 0xFF
