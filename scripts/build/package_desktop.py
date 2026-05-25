#!/usr/bin/env python3
"""Build + package a release-ready desktop binary for the host platform.

Runs under CI on macOS and Windows runners. The output lands in `dist/`:

  macOS arm64:  dist/projectMM-macos-arm64-vX.Y.Z.tar.gz
  Windows x64:  dist/projectMM-windows-x64-vX.Y.Z.zip

Each archive carries the executable + a short README.txt with run instructions.

Linux is intentionally not supported here — projectMM 1.0 ships ESP32 firmware
+ macOS + Windows desktop only. Linux desktop is on the 2.0 roadmap.

Both archives are unsigned; macOS users will see the Gatekeeper "downloaded
from internet" prompt on first run. Documented in the release notes.
"""

import json
import platform
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
# Per-host build dirs to match the ESP32 ``build/esp32-<board>/`` shape.
# Each host gets its own; CI runners use exactly one, but on a developer
# machine the layout means an experimental Linux build wouldn't clobber a
# macOS package job.
BUILD_DIR_MACOS = ROOT / "build" / "macos"
BUILD_DIR_WIN   = ROOT / "build" / "windows"
DIST_DIR = ROOT / "dist"


def read_version() -> str:
    meta = json.loads((ROOT / "library.json").read_text())
    return meta["version"]


def run(cmd: list[str]) -> None:
    print("+ " + " ".join(cmd))
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        sys.exit(r.returncode)


def configure_and_build_macos() -> Path:
    """Configure + build for macOS arm64. Returns the built binary path."""
    bdir = str(BUILD_DIR_MACOS.relative_to(ROOT))
    run([
        "cmake", "-B", bdir,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_OSX_ARCHITECTURES=arm64",
    ])
    run(["cmake", "--build", bdir, "--config", "Release", "-j"])
    binary = BUILD_DIR_MACOS / "projectMM"
    if not binary.exists():
        print(f"package_desktop: expected binary not found at {binary}")
        sys.exit(1)
    return binary


def configure_and_build_windows() -> Path:
    """Configure + build for Windows x64. Returns the built binary path."""
    bdir = str(BUILD_DIR_WIN.relative_to(ROOT))
    run([
        "cmake", "-B", bdir,
        "-G", "Visual Studio 17 2022", "-A", "x64",
        "-DCMAKE_BUILD_TYPE=Release",
    ])
    run(["cmake", "--build", bdir, "--config", "Release"])
    # MSVC multi-config places binaries under <build-dir>/Release/.
    binary = BUILD_DIR_WIN / "Release" / "projectMM.exe"
    if not binary.exists():
        # Some generators drop it directly under the build dir.
        fallback = BUILD_DIR_WIN / "projectMM.exe"
        if fallback.exists():
            return fallback
        print(f"package_desktop: expected binary not found at {binary}")
        sys.exit(1)
    return binary


def readme_text(version: str, platform_label: str) -> str:
    return (
        f"projectMM v{version} — {platform_label}\n"
        f"\n"
        f"Run: ./projectMM (macOS) or projectMM.exe (Windows)\n"
        f"Open: http://localhost:8080/\n"
        f"\n"
        f"macOS first run: Gatekeeper will prompt because the binary is\n"
        f"unsigned. Right-click → Open, or 'xattr -dr com.apple.quarantine\n"
        f"./projectMM' to clear the quarantine flag.\n"
        f"\n"
        f"Source: https://github.com/ewowi/projectMM\n"
    )


def package_macos(binary: Path, version: str) -> Path:
    DIST_DIR.mkdir(exist_ok=True)
    out = DIST_DIR / f"projectMM-macos-arm64-v{version}.tar.gz"
    readme = DIST_DIR / "_README.txt"
    readme.write_text(readme_text(version, "macOS arm64"))
    try:
        with tarfile.open(out, "w:gz") as tar:
            tar.add(binary, arcname="projectMM")
            tar.add(readme, arcname="README.txt")
    finally:
        readme.unlink(missing_ok=True)
    print(f"package_desktop: wrote {out}")
    return out


def package_windows(binary: Path, version: str) -> Path:
    DIST_DIR.mkdir(exist_ok=True)
    out = DIST_DIR / f"projectMM-windows-x64-v{version}.zip"
    readme = DIST_DIR / "_README.txt"
    readme.write_text(readme_text(version, "Windows x64"))
    try:
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.write(binary, arcname="projectMM.exe")
            zf.write(readme, arcname="README.txt")
    finally:
        readme.unlink(missing_ok=True)
    print(f"package_desktop: wrote {out}")
    return out


def main() -> int:
    version = read_version()
    system = platform.system()
    machine = platform.machine().lower()

    # Clean only THIS host's build dir so a configure-flag change picked
    # up by this run gets a fresh CMakeCache. We don't touch the other
    # host's dir; on CI each runner only ever sees its own anyway.
    host_build = BUILD_DIR_MACOS if system == "Darwin" else BUILD_DIR_WIN
    if host_build.exists():
        shutil.rmtree(host_build, ignore_errors=True)

    if system == "Darwin":
        if machine not in ("arm64", "aarch64"):
            print(f"package_desktop: unsupported macOS arch '{machine}'. "
                  f"projectMM 1.0 ships macOS arm64 only.")
            return 2
        binary = configure_and_build_macos()
        package_macos(binary, version)
        return 0

    if system == "Windows":
        binary = configure_and_build_windows()
        package_windows(binary, version)
        return 0

    print(f"package_desktop: host '{system}' not supported. "
          f"projectMM 1.0 ships macOS arm64 + Windows x64 only.")
    return 2


if __name__ == "__main__":
    sys.exit(main())
