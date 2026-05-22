#!/usr/bin/env python3
"""Generate src/core/version.h from library.json."""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
VERSION_FILE = ROOT / "library.json"
OUT_FILE = ROOT / "src" / "core" / "version.h"

data = json.loads(VERSION_FILE.read_text())
version = data["version"]

content = f'''#pragma once

// Auto-generated from library.json — do not edit
#define MM_VERSION "{version}"
#define MM_BUILD_DATE __DATE__ " " __TIME__
'''

# Only write if changed (avoid unnecessary rebuilds)
if OUT_FILE.exists() and OUT_FILE.read_text() == content:
    pass
else:
    OUT_FILE.write_text(content)
    print(f"Generated version.h: {version}")
