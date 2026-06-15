#!/usr/bin/env python3
"""Generate src/core/build_info.h.

Writes a single header carrying every compile-time identity fact the runtime
exposes through SystemModule:

  MM_VERSION       — semver, from library.json. Auto-generated section.
  MM_BUILD_DATE    — __DATE__ " " __TIME__, evaluated by the compiler.
  MM_FIRMWARE_NAME — set by the build system as a -D flag (see
                     scripts/build/build_esp32.py firmware_cmake_args() and
                     scripts/build/package_desktop.py). The header carries an
                     #ifndef "unknown" fallback for builds that didn't set it.
                     "Firmware" is the compiled-binary variant; the physical
                     board is a separate concept the device cannot self-identify.
                     See docs/architecture.md § Firmware vs board.
  MM_RELEASE       — release-channel tag (`latest`, `v1.0.0`), set by the
                     release workflow as a -D flag. #ifndef "" fallback for
                     local/dev builds (no channel). MM_VERSION = what code;
                     MM_RELEASE = which channel.

The generator rewrites the whole file from this template each time
library.json changes; the #ifndef defaults below are part of the template,
so they survive regeneration.
"""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
LIBRARY_JSON = ROOT / "library.json"
OUT_FILE = ROOT / "src" / "core" / "build_info.h"

data = json.loads(LIBRARY_JSON.read_text())
version = data["version"]

content = f'''#pragma once

// Auto-generated from library.json by scripts/build/generate_build_info.py
// -- do not edit by hand. Regenerated when library.json changes.

#define MM_VERSION    "{version}"
#define MM_BUILD_DATE __DATE__ " " __TIME__

// Compile-time identity from build flags. The build script that knows the
// value passes it as a -D, and SystemModule surfaces it on the device card
// (and the OTA path reads it to pick a matching release asset).
//
// "Firmware" here is the compiled-binary variant (esp32 / esp32-eth /
// esp32-16mb / esp32s3-n16r8) — see docs/architecture.md § Firmware
// vs board. The physical hardware ("board") is a separate concept the
// device cannot identify on its own.
//
//   ESP32:   scripts/build/build_esp32.py firmware_cmake_args() -> -DMM_FIRMWARE_NAME="<key>"
//   Desktop: scripts/build/package_desktop.py for release builds; local
//            CMake builds fall through to "unknown" today (no harm:
//            local builds aren't published).
#ifndef MM_FIRMWARE_NAME
#define MM_FIRMWARE_NAME "unknown"
#endif

// MM_RELEASE — the release-channel tag this binary was published under
// (`latest`, `v1.0.0`, `v1.0.0-rc2`). Set by the release workflow as a -D
// flag (same mechanism as MM_FIRMWARE_NAME). MM_VERSION is the semver from
// library.json — what code this is; MM_RELEASE is which channel it shipped
// on — a moving `latest` build and a tagged release can share a semver but
// differ in channel. Empty default: a local / dev build has no channel, and
// SystemModule shows just the bare semver in that case.
#ifndef MM_RELEASE
#define MM_RELEASE ""
#endif

namespace mm {{

constexpr const char* kVersion      = MM_VERSION;
constexpr const char* kBuildDate    = MM_BUILD_DATE;
constexpr const char* kFirmwareName = MM_FIRMWARE_NAME;
constexpr const char* kRelease      = MM_RELEASE;

}} // namespace mm
'''

# Force UTF-8 on both read and write — Python's default on Windows is cp1252,
# which can't encode anything outside ASCII. Even though the template above is
# ASCII today, pinning the encoding makes the script robust if a future edit
# slips a non-ASCII character into the comments.
if OUT_FILE.exists() and OUT_FILE.read_text(encoding="utf-8") == content:
    pass  # only write if changed (avoid unnecessary rebuilds)
else:
    OUT_FILE.write_text(content, encoding="utf-8")
    print(f"Generated build_info.h: version={version}")
