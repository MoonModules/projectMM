#!/usr/bin/env python3
"""Generate src/core/build_info.h.

Writes a single header carrying every compile-time identity fact the runtime
exposes through SystemModule:

  MM_VERSION       — semver, from library.json. Auto-generated section.
  MM_BUILD_DATE    — __DATE__ " " __TIME__, evaluated by the compiler.
  MM_BOARD_NAME    — set by the build system as a -D flag (see
                     scripts/build/build_esp32.py board_cmake_args() and
                     scripts/build/package_desktop.py). The header carries an
                     #ifndef "unknown" fallback for builds that didn't set it.

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
// — do not edit by hand. Regenerated when library.json changes.

#define MM_VERSION    "{version}"
#define MM_BUILD_DATE __DATE__ " " __TIME__

// Compile-time identity from build flags. The build script that knows the
// value passes it as a -D, and SystemModule surfaces it on the device card
// (and the future OTA path reads it to pick a matching release asset).
//
//   ESP32:   scripts/build/build_esp32.py board_cmake_args() → -DMM_BOARD_NAME="<key>"
//   Desktop: scripts/build/package_desktop.py for release builds; local
//            CMake builds fall through to "unknown" today (no harm —
//            local builds aren't published).
#ifndef MM_BOARD_NAME
#define MM_BOARD_NAME "unknown"
#endif

namespace mm {{

constexpr const char* kVersion   = MM_VERSION;
constexpr const char* kBuildDate = MM_BUILD_DATE;
constexpr const char* kBoardName = MM_BOARD_NAME;

}} // namespace mm
'''

# Only write if changed (avoid unnecessary rebuilds)
if OUT_FILE.exists() and OUT_FILE.read_text() == content:
    pass
else:
    OUT_FILE.write_text(content)
    print(f"Generated build_info.h: version={version}")
