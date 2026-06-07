#!/usr/bin/env python3
"""Check and set up ESP-IDF Python environment."""

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_esp32 import find_idf, IDF_SEARCH_PATHS

# Espressif ships install.sh for POSIX hosts and install.bat / install.ps1 for
# Windows. Both create the same `~/.espressif/python_env/...` venv and download
# the same toolchains — only the wrapper differs. install.bat is the broadest
# entry point on Windows (works in cmd.exe and PowerShell); .ps1 needs the
# execution policy unlocked, which we'd rather not assume.
INSTALL_SCRIPT_NAME = "install.bat" if sys.platform == "win32" else "install.sh"


def main():
    idf_path = find_idf()
    if not idf_path:
        print("ESP-IDF not found.")
        print("Install it from https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/")
        print("Searched: " + ", ".join(str(p) for p in IDF_SEARCH_PATHS))
        sys.exit(1)

    print(f"ESP-IDF found at {idf_path}")

    # Check version
    version_file = idf_path / "version.txt"
    if version_file.exists():
        print(f"Version: {version_file.read_text(encoding='utf-8').strip()}")

    # The shallow `git clone --depth 1` users typically run skips submodules,
    # but install.{sh,bat} needs the vendored tooling under `tools/idf_tools.py`
    # and the `components/*/` submodules. Run an idempotent submodule init —
    # already-initialized submodules are a fast no-op.
    print("Initializing ESP-IDF submodules (idempotent)...")
    r = subprocess.run(
        ["git", "submodule", "update", "--init", "--recursive", "--depth", "1"],
        cwd=str(idf_path))
    if r.returncode != 0:
        print("Submodule init failed.")
        sys.exit(r.returncode)

    install_script = idf_path / INSTALL_SCRIPT_NAME
    if not install_script.exists():
        print(f"{INSTALL_SCRIPT_NAME} not found at {install_script}")
        sys.exit(1)

    print(f"Running ESP-IDF {INSTALL_SCRIPT_NAME} (creates Python venv)...")
    # shell=True on Windows so cmd.exe interprets the .bat correctly.
    r = subprocess.run([str(install_script), "esp32"],
                       cwd=str(idf_path),
                       shell=(sys.platform == "win32"))
    if r.returncode != 0:
        print("Install failed.")
        sys.exit(r.returncode)

    print("\nESP-IDF setup complete. You can now build for ESP32.")

if __name__ == "__main__":
    main()
