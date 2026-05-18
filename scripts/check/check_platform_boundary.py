#!/usr/bin/env python3
"""Check that platform-specific code stays inside src/platform/."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

# Patterns that must only appear in src/platform/
FORBIDDEN_INCLUDES = [
    r'#include\s*<esp_',
    r'#include\s*<freertos/',
    r'#include\s*<driver/',
    r'#include\s*<hal/',
    r'#include\s*<soc/',
    r'#include\s*[<"]SDL',
    r'#include\s*[<"]wiringPi',
    r'#include\s*[<"]pigpio',
]

FORBIDDEN_IFDEFS = [
    r'#ifn?def\s+(?:ESP_PLATFORM|CONFIG_IDF|__APPLE__|__linux__|_WIN32)',
    r'#if\s+defined\s*\(\s*(?:ESP_PLATFORM|CONFIG_IDF|__APPLE__|__linux__|_WIN32)',
]

ALL_PATTERNS = [re.compile(p) for p in FORBIDDEN_INCLUDES + FORBIDDEN_IFDEFS]
EXTENSIONS = {".h", ".hpp", ".cpp", ".c"}


def check_file(path: Path) -> list[str]:
    violations = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return violations

    for i, line in enumerate(text.splitlines(), 1):
        for pattern in ALL_PATTERNS:
            if pattern.search(line):
                violations.append(f"  {path}:{i}: {line.strip()}")
    return violations


def main():
    if not SRC.exists():
        print("src/ directory not found. Nothing to check.")
        sys.exit(0)

    platform_dir = SRC / "platform"
    violations = []

    for path in SRC.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix not in EXTENSIONS:
            continue
        # Skip files inside src/platform/
        try:
            path.relative_to(platform_dir)
            continue
        except ValueError:
            pass

        violations.extend(check_file(path))

    if violations:
        print(f"Platform boundary violations ({len(violations)}):\n")
        for v in violations:
            print(v)
        sys.exit(1)
    else:
        print("Platform boundary check passed.")
        sys.exit(0)


if __name__ == "__main__":
    main()
