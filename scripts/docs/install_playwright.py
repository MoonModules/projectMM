#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["playwright"]
# ///
"""One-time setup: install the Playwright Chromium browser.

Run this once before using Screenshot Modules. Safe to re-run — Playwright
skips the download if the browser is already installed.

Usage:
    uv run scripts/docs/install_playwright.py
"""

import subprocess
import sys


def main() -> int:
    print("Installing Playwright Chromium browser …")
    r = subprocess.run(
        [sys.executable, "-m", "playwright", "install", "chromium"],
        check=False,
    )
    if r.returncode == 0:
        print("Playwright Chromium installed. Ready to run Screenshot Modules.")
    else:
        print("Installation failed — check the output above.")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
