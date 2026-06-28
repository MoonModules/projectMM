#!/usr/bin/env python3
"""Check and set up ESP-IDF Python environment."""

import argparse
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

# The ESP-IDF commit every target (classic ESP32, S3, P4, S31) has been
# validated against — a commit on the `release/v6.1` branch, the earliest IDF
# line that carries the esp32s31 preview target. The script can't *clone* the
# IDF for you (the dev does the initial clone per docs/building.md), but when the
# installed checkout drifts from this pin it offers to move it (git checkout +
# submodule sync) so a fresh shallow clone landing on a newer dev-branch commit
# — exactly what turns a green build red with no code change — converges back to
# the validated commit. Pass --no-checkout to keep the warn-only behaviour (a
# dev deliberately migrating off this snapshot to a newer release re-tests, not
# auto-reverts).
PINNED_IDF_COMMIT = "0d9287800812c95662921c2c5e812023939e3d58"
PINNED_IDF_VERSION = "v6.1-dev-5215-g0d928780081"


def _installed_idf_commit(idf_path: Path) -> str:
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(idf_path),
                           capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else ""
    except OSError:
        return ""


def _checkout_pinned(idf_path: Path) -> bool:
    """Move the installed IDF onto PINNED_IDF_COMMIT (checkout + submodule sync).

    Returns True on success. The pinned commit must already be fetched (the dev
    cloned the right branch per docs/building.md); this only moves HEAD onto it
    and re-syncs submodules — it does not fetch or clone.
    """
    co = subprocess.run(["git", "checkout", PINNED_IDF_COMMIT], cwd=str(idf_path))
    if co.returncode != 0:
        print(f"   Checkout failed — the pinned commit may not be fetched yet. "
              f"In {idf_path}: git fetch origin release/v6.1, then re-run.")
        return False
    # The new commit points its submodules at different SHAs; sync them so the
    # build sees the matching component sources.
    subprocess.run(["git", "submodule", "update", "--init", "--recursive",
                    "--depth", "1"], cwd=str(idf_path))
    return True


def main():
    parser = argparse.ArgumentParser(description="Set up the ESP-IDF Python environment.")
    parser.add_argument("--no-checkout", action="store_true",
                        help="Only warn on IDF commit drift; do not offer to check "
                             "out the pinned commit (for a dev deliberately "
                             "migrating to a newer release).")
    args = parser.parse_args()

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

    # Drift warning + convergence: the build was validated against a specific IDF
    # commit, and the dev branch this snapshot lives on moves. A mismatch isn't
    # fatal (you may be deliberately migrating), but it must be visible — and by
    # default we offer to move the checkout onto the pin so a new dev converges on
    # the validated commit. --no-checkout keeps it warn-only.
    installed = _installed_idf_commit(idf_path)
    if installed and installed != PINNED_IDF_COMMIT:
        print(f"\n⚠  IDF commit drift: installed {installed[:12]} != "
              f"pinned {PINNED_IDF_COMMIT[:12]} ({PINNED_IDF_VERSION}).")
        print("   Builds were validated against the pinned commit. If a build "
              "fails unexpectedly, this is the first suspect.")
        if args.no_checkout:
            print(f"   To pin: (cd {idf_path} && git checkout {PINNED_IDF_COMMIT})\n")
        else:
            answer = input(f"   Check out the pinned commit now? [Y/n] ").strip().lower()
            if answer in ("", "y", "yes"):
                if _checkout_pinned(idf_path):
                    print(f"   Checked out {PINNED_IDF_COMMIT[:12]} ({PINNED_IDF_VERSION}).\n")
            else:
                print("   Keeping the current checkout (drift unresolved).\n")

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
