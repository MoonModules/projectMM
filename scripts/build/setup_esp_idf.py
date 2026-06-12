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

# The ESP-IDF commit every target (classic ESP32, S3, P4) has been validated
# against. This script can't move an existing checkout for you (it doesn't own
# the clone), but it warns loudly when the installed IDF differs — a silent
# `git pull` or a fresh shallow clone landing on a newer dev-branch commit is
# exactly what turns a green build red with no code change (see
# docs/backlog/backlog.md "ESP-IDF version pinning"). To pin: in ~/esp/esp-idf,
# `git fetch && git checkout <commit>`. Migrating off this dev snapshot to a
# stable tag (v6.1 lands 2026-07-31) is a deliberate re-test pass, not a pull.
PINNED_IDF_COMMIT = "d1b91b79b5ff12d9d4b21fe1cf5406ab6044b8ff"
PINNED_IDF_VERSION = "v6.1-dev-399-gd1b91b79b5"


def _installed_idf_commit(idf_path: Path) -> str:
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(idf_path),
                           capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else ""
    except OSError:
        return ""


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

    # Drift warning: the build was validated against a specific IDF commit, and
    # the dev branch this snapshot lives on moves. A mismatch isn't fatal (you
    # may be deliberately migrating), but it must be visible.
    installed = _installed_idf_commit(idf_path)
    if installed and installed != PINNED_IDF_COMMIT:
        print(f"\n⚠  IDF commit drift: installed {installed[:12]} != "
              f"pinned {PINNED_IDF_COMMIT[:12]} ({PINNED_IDF_VERSION}).")
        print("   Builds were validated against the pinned commit. If a build "
              "fails unexpectedly, this is the first suspect.")
        print(f"   To pin: (cd {idf_path} && git checkout {PINNED_IDF_COMMIT})\n")

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
