#!/usr/bin/env python3
"""Locally preview the web installer at docs/install/index.html.

Stages a small directory (the install page + the shared release-picker
module) and serves it with `python -m http.server`. Two modes depending
on what's been built locally:

  - **render-only** (no `build/esp32-*/projectMM.bin` present): the
    picker populates against the real GitHub Releases API, dropdowns
    work, but clicking Install fails because the local server has no
    `releases/` tree. Equivalent to "Recipe A" in docs/install/README.md.
    Useful for iterating on HTML/CSS/JS without tagging a release.

  - **flash-ready** (at least one local ESP32 build exists): the
    script additionally stages every `build/esp32-*/projectMM.bin` it
    finds into `releases/local-dev/` and generates matching Pages-
    relative manifests via generate_manifest.py — the same code path
    .github/workflows/release.yml uses. The picker shows `local-dev`
    as the newest release; clicking Install flashes the chip + opens
    the ESP Web Tools Improv WiFi modal, end-to-end.

The flash-ready mode is the developer's test ground for the install
flow before deploying to GitHub Pages: any change to the installer
page or release-picker.js can be verified against a real ESP32 over
`http://localhost:8000/` (Web Serial works on localhost without the
secure-origin requirement that gates the public site).

Long-running. MoonDeck shows a Stop button while this is up; pressing
Stop kills the python process (matched by script name via pkill).
"""

import http.server
import socketserver
import subprocess
import sys
import shutil
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
INSTALL_DIR = ROOT / "docs" / "install"
PICKER_JS = ROOT / "src" / "ui" / "release-picker.js"
STAGE_DIR = ROOT / "build" / "install-preview"
BUILD_ROOT = ROOT / "build"
GENERATE_MANIFEST = ROOT / "scripts" / "build" / "generate_manifest.py"
# Stage under the tag the picker WILL surface from the live GitHub Releases
# API: `latest` is always the newest CI build. Local bins replace the real
# `latest` bins for the duration of the preview — that's the point, we want
# to flash what we've built locally, not what's on GitHub. The picker shows
# `latest` (and any other real tags) regardless; clicking Install on `latest`
# resolves the manifest path to our staged copy, served same-origin from the
# preview server. Other tags (`v1.0.0-rc2` etc.) the picker offers will 404
# in preview because we don't stage them — clearly visible failure if the
# user picks the wrong one. (Picking a `v*` tag in preview is a developer
# mistake; the picker defaults to the newest stable, so if no stable
# release exists yet the default lands on `latest`, which works.)
LOCAL_TAG = "latest"
LOCAL_VERSION = "local-dev"
PORT = 8000


def stage_install_page():
    """Refresh the preview dir with the current install-page sources.

    Re-staging on every run keeps the local copy honest — if you edit
    index.html or any sibling JS while the server is up, restart via
    the Stop/Run cycle to pick up changes (or rely on the `?nocache=1`
    query parameter the picker honours).

    Stages every browser-loadable file under docs/install/ (.html / .js
    / .css), not just index.html, so future additions land in the
    preview for free. The release-picker module lives separately under
    src/ui/ (it's shared with the on-device UI) and is copied over
    alongside. README.md is skipped — docs only.
    """
    if STAGE_DIR.exists():
        shutil.rmtree(STAGE_DIR)
    STAGE_DIR.mkdir(parents=True)

    if not INSTALL_DIR.exists():
        print(f"ERROR: install source not found at {INSTALL_DIR}", file=sys.stderr)
        sys.exit(1)
    if not PICKER_JS.exists():
        print(f"ERROR: release-picker.js not found at {PICKER_JS}", file=sys.stderr)
        sys.exit(1)

    # Mirror release.yml's "cp -r docs/install/. pages/install/" step:
    # take every runtime file in docs/install/ (so devices.js etc. land too,
    # not just index.html). README.md is docs, skip it; .md in general is
    # docs not runtime.
    for src in INSTALL_DIR.iterdir():
        if src.is_file() and src.suffix.lower() in (".html", ".js", ".css"):
            shutil.copy(src, STAGE_DIR / src.name)
    # release-picker.js lives in src/ui/ (shared with the on-device UI).
    shutil.copy(PICKER_JS, STAGE_DIR / "release-picker.js")


def find_local_builds() -> list[Path]:
    """Return every `build/esp32-*/` dir that has a flashable projectMM.bin.

    Multiple firmware variants can coexist on disk (each in its own
    `build/esp32-<firmware>/` dir per build_esp32.py). We stage them all
    so the release-picker can offer the right one based on the device's
    reported firmware key during Improv probe — same shape as production
    where every release publishes every variant.
    """
    if not BUILD_ROOT.exists():
        return []
    return sorted(
        p for p in BUILD_ROOT.glob("esp32-*")
        if (p / "projectMM.bin").exists()
    )


def stage_local_builds(builds: list[Path]) -> list[str]:
    """Copy bins + generate Pages-relative manifests for each local build.

    Mirrors .github/workflows/release.yml's "Stage release artifacts" +
    "Generate ESP Web Tools manifests" steps, just into the preview
    staging dir and using a fixed `local-dev` tag instead of a git tag.
    Returns the list of firmware keys staged (used in the boot log).
    """
    releases_dir = STAGE_DIR / "releases" / LOCAL_TAG
    releases_dir.mkdir(parents=True, exist_ok=True)

    staged: list[str] = []
    for build_dir in builds:
        # build_dir.name is "esp32-<firmware>"; strip the prefix.
        firmware = build_dir.name[len("esp32-"):]
        prefix = f"firmware-{firmware}-v{LOCAL_VERSION}"

        # The four .bin files that go alongside a firmware in the release.
        # Mirrors release.yml line ~116-119 exactly.
        try:
            shutil.copy(build_dir / "projectMM.bin",
                        releases_dir / f"{prefix}.bin")
            shutil.copy(build_dir / "bootloader" / "bootloader.bin",
                        releases_dir / f"{prefix}-bootloader.bin")
            shutil.copy(build_dir / "partition_table" / "partition-table.bin",
                        releases_dir / f"{prefix}-partition-table.bin")
            shutil.copy(build_dir / "ota_data_initial.bin",
                        releases_dir / f"{prefix}-ota-data.bin")
        except FileNotFoundError as e:
            # Partial build (bootloader / partition-table missing) — skip this
            # firmware rather than half-stage it, the picker would offer it
            # but Install would fail mid-flash.
            print(f"==> skip {firmware}: missing {e.filename}", file=sys.stderr)
            continue

        # Pages-relative manifest (release-url = "." → siblings of the manifest).
        # Generate via the same script production uses so the manifest shape
        # stays in sync with what ESP Web Tools expects.
        manifest_path = releases_dir / f"manifest-{firmware}.json"
        flasher_args = build_dir / "flasher_args.json"
        if not flasher_args.exists():
            print(f"==> skip {firmware}: missing flasher_args.json", file=sys.stderr)
            continue
        result = subprocess.run(
            [sys.executable, str(GENERATE_MANIFEST),
             "--firmware", firmware,
             "--version", LOCAL_VERSION,
             "--release-url", ".",
             "--flasher-args", str(flasher_args),
             "--out", str(manifest_path)],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            print(f"==> skip {firmware}: manifest generation failed:\n{result.stderr}",
                  file=sys.stderr)
            continue
        staged.append(firmware)

    return staged


def _port_in_use(port: int) -> bool:
    """True if something is already listening on `port` on this host. Used
    as a pre-flight before the staging step nukes STAGE_DIR — re-running
    while an old preview is up would yank the running server's cwd and
    leave it serving from a deleted directory (requests then hang)."""
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.settimeout(0.2)
        return s.connect_ex(("127.0.0.1", port)) == 0
    finally:
        s.close()


def main() -> int:
    if _port_in_use(PORT):
        print(f"ERROR: port {PORT} is in use. Another preview already running? "
              f"Stop it first (MoonDeck → Stop, or `pkill -f preview_installer.py`).",
              file=sys.stderr)
        return 1

    stage_install_page()

    print(f"==> staged {STAGE_DIR}")

    # Look for local ESP32 builds and stage them as the `local-dev` release.
    # When none are found, the preview runs in render-only mode (picker
    # works against the real GitHub API, Install fails for lack of bins).
    local_builds = find_local_builds()
    if local_builds:
        firmwares = stage_local_builds(local_builds)
        if firmwares:
            print(f"==> staged local-dev release with firmwares: {', '.join(firmwares)}")
            print(f"    pick the `local-dev` tag in the picker to flash a USB-connected ESP32")
        else:
            print(f"==> no firmwares could be staged (all skipped — see warnings above)")
            print(f"    falling back to render-only mode")
    else:
        print(f"==> no local ESP32 builds found under {BUILD_ROOT.relative_to(ROOT)}")
        print(f"    render-only mode — `Install` will fail")
        print(f"    run `uv run scripts/build/build_esp32.py --firmware <variant>` first to enable end-to-end flash")

    print(f"==> serving at http://localhost:{PORT}/")
    print("    open in Chrome / Edge / Opera (Web Serial requires one of these)")
    print("    add ?nocache=1 to bypass the picker's 5-min sessionStorage cache")
    print()
    sys.stdout.flush()

    handler = http.server.SimpleHTTPRequestHandler

    class QuietTCPServer(socketserver.ThreadingTCPServer):
        allow_reuse_address = True   # avoids "Address already in use" on quick restart
        daemon_threads = True

    # cwd=STAGE_DIR equivalent: serve files from STAGE_DIR by chdir'ing first.
    # SimpleHTTPRequestHandler serves from the current working directory.
    import os
    os.chdir(STAGE_DIR)

    try:
        with QuietTCPServer(("", PORT), handler) as httpd:
            httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n==> stopped")
    except OSError as e:
        if e.errno == 48 or "Address already in use" in str(e):
            print(f"ERROR: port {PORT} is in use. Another preview already running?",
                  file=sys.stderr)
            return 1
        raise
    return 0


if __name__ == "__main__":
    sys.exit(main())
