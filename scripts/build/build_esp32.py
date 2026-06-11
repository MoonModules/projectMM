#!/usr/bin/env python3
"""Build the ESP32 target for a specific firmware variant.

"Firmware" here is the compiled binary variant (chip + radios/peripherals +
sdkconfig fragments) — separate from "board" (physical hardware: PCB, PHY,
USB-serial, PSRAM). See docs/architecture.md § Firmware vs board.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ESP32_DIR = ROOT / "esp32"

# Common ESP-IDF install locations
IDF_SEARCH_PATHS = [
    Path.home() / "esp" / "esp-idf",
    Path.home() / ".espressif" / "esp-idf",
    Path("/opt/esp-idf"),
]

# Components to drop from an Ethernet-only build. ESP-IDF v6.x has no
# CONFIG_ESP_WIFI_ENABLED switch (the symbol is non-settable, forced y on
# WiFi-capable SoCs), so WiFi is removed via EXCLUDE_COMPONENTS instead.
# All consumers of these use *optional* requires, so excluding them links
# cleanly as long as our own code never references esp_wifi (it doesn't —
# the WiFi platform functions are #ifdef-stubbed in the eth-only build).
#
# NOTE: esp_phy is NOT excluded — it provides RF/clock init the ESP32 EMAC
# (Ethernet RMII) depends on. Excluding it leaves Ethernet stuck "started"
# with no link. Only the genuinely WiFi-side components are dropped.
ETH_ONLY_EXCLUDE = ["esp_wifi", "wpa_supplicant", "esp_coex"]

# Firmware catalogue. Each entry describes one shipping firmware variant.
# Keys combine chip name + feature flags + (for SKU-sensitive chips) module:
#   esp32           — ESP32 classic, WiFi only
#   esp32-eth       — ESP32 classic, Ethernet only (WiFi compiled out)
#   esp32-eth-wifi  — ESP32 classic, Ethernet + WiFi (both available)
#   esp32s3-n16r8   — ESP32-S3 DevKitC-1 with the N16R8 module
#                     (16 MB flash, 8 MB octal PSRAM). Other S3 SKUs (N8R2,
#                     N8R8, …) get their own key — the sdkconfig fragment
#                     encodes flash size + partition table + PSRAM mode,
#                     which differ per SKU.
# The Ethernet variants bake in Olimex ESP32-Gateway pin defaults
# (sdkconfig.defaults.eth). Runtime PHY/pin selection is on the 2.0 roadmap.
FIRMWARES: dict[str, dict] = {
    "esp32": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults"],
        "eth_only": False,
        "description": "ESP32 classic — WiFi only",
    },
    "esp32-16mb": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.16mb"],
        "eth_only": False,
        "description": "ESP32 classic with 16 MB flash — WiFi only. Same silicon "
                       "as `esp32`; the 4 MB binary runs on these boards too, this "
                       "variant just uses the extra flash for bigger OTA slots + "
                       "filesystem (Serg boards, QuinLED Dig-Octa).",
    },
    "esp32-eth": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.eth"],
        "eth_only": True,
        "description": "ESP32 classic — Ethernet only (Olimex pins, WiFi compiled out)",
    },
    "esp32-eth-wifi": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.eth"],
        "eth_only": False,
        "description": "ESP32 classic — Ethernet + WiFi (Olimex pins)",
    },
    "esp32s3-n16r8": {
        "chip": "esp32s3",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s3-n16r8"],
        "eth_only": False,
        "description": "ESP32-S3 DevKitC-1 (N16R8: 16 MB flash, 8 MB octal PSRAM) — WiFi only",
    },
    "esp32s3-n8r8": {
        "chip": "esp32s3",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s3-n8r8"],
        "eth_only": False,
        "description": "ESP32-S3 (N8R8: 8 MB flash, 8 MB octal PSRAM) — WiFi only. "
                       "Half the flash of N16R8; the N16R8 binary overruns an 8 MB "
                       "board, so N8R8 boards (LightCrafter etc.) need this variant.",
    },
    "esp32p4-eth": {
        "chip": "esp32p4",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32p4-eth"],
        "eth_only": True,
        "description": "Waveshare ESP32-P4-NANO — Ethernet only (IP101 PHY). WiFi "
                       "needs the on-board ESP32-C6 co-processor (esp-hosted), not "
                       "yet wired; round 3 adds it.",
    },
}

# Deprecated --profile values → firmware, kept one release for callers that
# still pass --profile. Remove once external tooling has migrated.
PROFILE_ALIASES = {
    "default": "esp32",
    "eth-only": "esp32-eth",
}


def find_idf() -> Path | None:
    """Find ESP-IDF installation. Checks IDF_PATH env, then common locations."""
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        p = Path(idf_path)
        if (p / "tools" / "idf.py").exists():
            return p

    for p in IDF_SEARCH_PATHS:
        if (p / "tools" / "idf.py").exists():
            return p
    return None


# Python venv layout differs between platforms — POSIX puts the interpreter in
# `<venv>/bin/python`; Windows in `<venv>/Scripts/python.exe`. Same for the
# toolchain `bin` dirs ESP-IDF unpacks under ~/.espressif/tools (POSIX) vs
# %USERPROFILE%\.espressif\tools\…\Scripts (Windows for some tools, `bin` for
# the GCC toolchains). The constants below capture the per-platform split.
_VENV_BIN = "Scripts" if sys.platform == "win32" else "bin"
_PYTHON_EXE = "python.exe" if sys.platform == "win32" else "python"


def find_idf_python() -> Path | None:
    """Find the ESP-IDF Python venv. Prefers most recently modified."""
    venv_dir = Path.home() / ".espressif" / "python_env"
    if not venv_dir.exists():
        return None
    candidates = []
    for d in venv_dir.iterdir():
        python = d / _VENV_BIN / _PYTHON_EXE
        if python.exists():
            candidates.append((d.stat().st_mtime, d))
    if candidates:
        candidates.sort(reverse=True)
        return candidates[0][1]
    return None


def idf_version(idf_path: Path) -> str:
    """Extract a clean semver from the IDF version string."""
    version_file = idf_path / "version.txt"
    if version_file.exists():
        raw = version_file.read_text(encoding="utf-8").strip()
        # Extract major.minor.patch from strings like "v6.1-dev-399-gd1b91b79b5"
        m = re.match(r"v?(\d+\.\d+)(?:\.(\d+))?", raw)
        if m:
            return f"{m.group(1)}.{m.group(2) or '0'}"
    return "5.4.0"  # safe fallback


def idf_env(idf_path: Path) -> dict:
    """Build the environment for running idf.py."""
    env = dict(os.environ)
    env["IDF_PATH"] = str(idf_path)
    env["ESP_IDF_VERSION"] = idf_version(idf_path)
    # ESP-IDF's Python tooling refuses to run on a non-UTF-8 locale. Windows
    # defaults to cp1252 (locale "English_Netherlands.1252" etc.), so idf.py
    # bails with "Support for Unicode is required". PYTHONUTF8=1 (PEP 540)
    # forces Python into UTF-8 mode regardless of the system locale, and
    # PYTHONIOENCODING covers the stdin/stdout/stderr streams.
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"

    venv_path = find_idf_python()
    if venv_path:
        env["IDF_PYTHON_ENV_PATH"] = str(venv_path)

    # IDF's post-build gen_gdbinit.py reads ESP_ROM_ELF_DIR (export.sh sets it;
    # this hand-built env must too). The step only re-runs when its inputs
    # change, so the missing variable failed builds intermittently.
    rom_elfs = Path.home() / ".espressif" / "tools" / "esp-rom-elfs"
    if rom_elfs.exists():
        versions = sorted((d for d in rom_elfs.iterdir() if d.is_dir()), reverse=True)
        if versions:
            env["ESP_ROM_ELF_DIR"] = str(versions[0]) + os.sep

    # Build PATH: venv bin + IDF tools + toolchains + existing PATH
    extra_paths = []

    if venv_path:
        extra_paths.append(str(venv_path / _VENV_BIN))

    extra_paths.append(str(idf_path / "tools"))

    # Add toolchain paths from ~/.espressif/tools. Tool layout varies — POSIX
    # tools have `bin/` subdirs (xtensa-esp-elf, cmake), Windows tools often
    # don't (ninja, ccache, idf-exe ship the .exe at the version-dir root, or
    # inside a single product-named subdir). Add both: the version dir itself
    # (catches flat layouts) plus any nested `bin/` subdir (catches POSIX
    # layouts). Together this covers every tool IDF installs on either host.
    tools_dir = Path.home() / ".espressif" / "tools"
    if tools_dir.exists():
        for tool in tools_dir.iterdir():
            if tool.is_dir():
                for version_dir in sorted(tool.iterdir(), reverse=True):
                    extra_paths.append(str(version_dir))
                    bin_dirs = list(version_dir.rglob("bin"))
                    if bin_dirs:
                        extra_paths.append(str(bin_dirs[0]))
                    break

    env["PATH"] = os.pathsep.join(extra_paths + [env.get("PATH", "")])
    return env


def idf_cmd(idf_path: Path) -> list[str]:
    """Return the command to invoke idf.py via the venv Python.

    On Windows the entry point is `_idf_win_shim.py` instead of idf.py
    directly — the shim calls `locale.setlocale(LC_ALL, "en_US.UTF-8")`
    BEFORE idf.py runs, which is the only way to make IDF's locale check
    (`locale.getlocale()`) pass on Windows installs whose system locale
    is non-UTF-8 (e.g. Dutch / German / French). See the shim's docstring.
    """
    venv_path = find_idf_python()
    python_exe = (str(venv_path / _VENV_BIN / _PYTHON_EXE)
                  if venv_path else "python")
    if sys.platform == "win32":
        shim = Path(__file__).resolve().parent / "_idf_win_shim.py"
        return [python_exe, str(shim)]
    return [python_exe, str(idf_path / "tools" / "idf.py")]


def firmware_cmake_args(firmware: str, release: str = "") -> list[str]:
    """Extra -D cache args for the requested firmware.

    `release` is the release-channel tag (e.g. "latest", "v1.0.0") to burn
    into the binary as MM_RELEASE. Empty for local builds — SystemModule
    then shows the bare semver with no channel suffix.
    """
    spec = FIRMWARES[firmware]
    fragments = ";".join(spec["fragments"])
    args = [f"-DSDKCONFIG_DEFAULTS={fragments}"]
    # Burn the firmware key into the binary so SystemModule can report it and
    # the OTA path can pick the matching release asset (every release ships
    # one .bin per firmware key — see release.yml).
    args.append(f'-DMM_FIRMWARE_NAME="{firmware}"')
    # Burn the release-channel tag too, when the build pipeline supplies one.
    # Same -D mechanism; empty default left to build_info.h's #ifndef so a
    # local build needs no flag.
    if release:
        args.append(f'-DMM_RELEASE="{release}"')
    if spec["eth_only"]:
        # Drop the WiFi components from the link, and tell our code to compile
        # out the WiFi paths (MM_ETH_ONLY → esp32/main/CMakeLists.txt).
        args.append("-DEXCLUDE_COMPONENTS=" + ";".join(ETH_ONLY_EXCLUDE))
        args.append("-DMM_ETH_ONLY=1")
    # Firmwares that don't enable the EMAC have no on-chip Ethernet headers
    # (`eth_esp32_emac_config_t`, …), so platform_esp32.cpp's ethInit() won't
    # compile — set MM_NO_ETH and the source provides stubs instead. A variant
    # "has Ethernet" when any fragment carries an EMAC-enabling sdkconfig: the
    # classic Olimex fragment is `sdkconfig.defaults.eth` (".eth"); board-specific
    # ones append "-eth" (e.g. ".esp32p4-eth"). Match either so a new eth board
    # doesn't silently stub Ethernet out.
    has_eth_fragment = any(f.endswith(".eth") or f.endswith("-eth")
                           for f in spec["fragments"])
    if not has_eth_fragment:
        args.append("-DMM_NO_ETH=1")
    return args


def resolve_firmware(args: argparse.Namespace) -> str:
    """Resolve the firmware name from --firmware or the deprecated --profile alias."""
    if args.firmware:
        if args.firmware not in FIRMWARES:
            valid = ", ".join(sorted(FIRMWARES))
            print(f"Unknown --firmware '{args.firmware}'. Choose one of: {valid}")
            sys.exit(2)
        return args.firmware

    if args.profile:
        alias = PROFILE_ALIASES.get(args.profile)
        if not alias:
            print(f"Unknown --profile '{args.profile}'. "
                  f"Use --firmware instead (one of: {', '.join(sorted(FIRMWARES))}).")
            sys.exit(2)
        print(f"--profile is deprecated; use --firmware {alias} instead.")
        return alias

    # No flag → keep the prior default behaviour (WiFi-only ESP32 classic).
    return "esp32"


def build_dir_for(firmware: str) -> Path:
    """Return the per-firmware build directory.

    Each firmware variant gets its own subdir of ``<ROOT>/build/`` so multiple
    variants can coexist on disk — switching firmwares no longer forces a
    clean rebuild. The ``esp32-`` prefix namespaces ESP32 firmware keys away
    from desktop targets (``build/macos/``, ``build/linux/``,
    ``build/windows/``) that share the same root. Common-patterns rationale:
    CMake / idf.py ``-B <dir>`` is the documented mechanism for parallel
    build dirs; the bespoke choice here is just the naming.
    """
    return ROOT / "build" / f"esp32-{firmware}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--env", help="ESP32 chip type (legacy; derived from --firmware)")
    parser.add_argument("--firmware", choices=sorted(FIRMWARES),
                        help="Firmware variant. One of: " + ", ".join(sorted(FIRMWARES)))
    parser.add_argument("--profile", choices=["default", "eth-only"],
                        help="Deprecated alias for --firmware. Use --firmware instead.")
    parser.add_argument("--release", default="",
                        help="Release-channel tag to burn into the binary as "
                             "MM_RELEASE (e.g. 'latest', 'v1.0.0'). Set by the "
                             "release workflow; omit for local builds.")
    args = parser.parse_args()

    firmware = resolve_firmware(args)
    chip = FIRMWARES[firmware]["chip"]
    # --env, if supplied, must agree with the firmware's chip
    if args.env and args.env != chip:
        print(f"--env {args.env} conflicts with --firmware {firmware} (chip: {chip}). "
              f"Drop --env or pass --firmware for a different chip.")
        sys.exit(2)

    if not ESP32_DIR.exists():
        print(f"ESP32 project directory not found: {ESP32_DIR}")
        sys.exit(1)

    idf_path = find_idf()
    if not idf_path:
        print("ESP-IDF not found. Install it or set IDF_PATH.")
        print("Searched: " + ", ".join(str(p) for p in IDF_SEARCH_PATHS))
        sys.exit(1)

    print(f"Using ESP-IDF at {idf_path}")
    env = idf_env(idf_path)
    cmd = idf_cmd(idf_path)

    build_dir = build_dir_for(firmware)
    # -B points idf.py at the per-firmware build dir. -DSDKCONFIG keeps each
    # firmware's sdkconfig inside its own build dir too — without this idf.py
    # writes `esp32/sdkconfig` at the project root, and switching firmwares
    # poisons it ("project sdkconfig was generated for target X, but
    # CMakeCache contains Y"). Per-build-dir sdkconfig is the IDF-supported
    # way to do parallel builds; CMake forwards the variable into the
    # build component manager. Absolute paths are necessary for SDKCONFIG
    # because CMake resolves it relative to the build dir, not the project.
    sdkconfig_path = build_dir / "sdkconfig"
    b_arg = [
        "-B", str(build_dir),
        "-DSDKCONFIG=" + str(sdkconfig_path),
    ]

    # First-time build for this firmware: idf.py needs `set-target` before
    # `build` so sdkconfig gets seeded from SDKCONFIG_DEFAULTS. On subsequent
    # builds the per-build-dir sdkconfig already has the chip pinned, so
    # set-target is skipped — switching to another firmware uses a different
    # build_dir entirely, so its sdkconfig is untouched.
    extra = firmware_cmake_args(firmware, args.release)
    if not build_dir.exists():
        print(f"Setting target to {chip} (firmware: {firmware}, build dir: "
              f"{build_dir.relative_to(ROOT)})...")
        r = subprocess.run(cmd + b_arg + extra + ["set-target", chip],
                           cwd=ESP32_DIR, env=env)
        if r.returncode != 0:
            sys.exit(r.returncode)

    print(f"Building for {chip} (firmware: {firmware})...")
    r = subprocess.run(cmd + b_arg + extra + ["build"], cwd=ESP32_DIR, env=env)
    if r.returncode != 0:
        sys.exit(r.returncode)

    # Show flash/RAM usage summary
    subprocess.run(cmd + b_arg + ["size"], cwd=ESP32_DIR, env=env)


if __name__ == "__main__":
    main()
