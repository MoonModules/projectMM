#!/usr/bin/env python3
"""Build the ESP32 target for a specific firmware variant.

"Firmware" here is the compiled binary variant (chip + radios/peripherals +
sdkconfig fragments) — separate from "board" (physical hardware: PCB, PHY,
USB-serial, PSRAM). See docs/architecture.md § Firmware vs board.
"""

import argparse
import os
import re
import shutil
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
#
# NOTE on the P4 co-processor components (esp_hosted / esp_wifi_remote / eppp_link):
# the `rules: target == esp32p4` gate in main/idf_component.yml pulls them for ANY
# esp32p4 build, including the WiFi-less esp32p4-eth, because manifest rules can't
# see our eth-only flag. EXCLUDE_COMPONENTS does NOT drop them (the component
# manager resolves the managed dependency before the exclude applies). It's a
# *build-time* cost only: the linker dead-strips the unused code, so they add ~0
# bytes of flash to esp32p4-eth (our coprocessorWifi() is the empty stub there, so
# no esp_hosted symbol is referenced — confirmed: their .text size is 0x0 in the
# .map). Left as-is rather than fought; see docs/backlog/.
ETH_ONLY_EXCLUDE = ["esp_wifi", "wpa_supplicant", "esp_coex"]

# Firmware catalogue. Each entry describes one shipping firmware variant.
# Keys combine chip name + feature flags + (for SKU-sensitive chips) module:
#   esp32           — ESP32 classic, WiFi + Ethernet (RMII; eth comes up only
#                     when a PHY is present, pins per board from deviceModels.json)
#   esp32-eth       — ESP32 classic, Ethernet only (WiFi compiled out — smaller)
#   esp32s3-n16r8   — ESP32-S3 DevKitC-1 with the N16R8 module
#                     (16 MB flash, 8 MB octal PSRAM). Other S3 SKUs (N8R2,
#                     N8R8, …) get their own key — the sdkconfig fragment
#                     encodes flash size + partition table + PSRAM mode,
#                     which differ per SKU.
# The Ethernet driver is compiled into each chip's firmware (RMII EMAC for
# classic/P4 via sdkconfig.defaults.eth, W5500 SPI for S3 via .eth-spi);
# which PHY/pins a given board uses is runtime config (deviceModels.json →
# NetworkModule → ethInit), so one binary per chip serves every board.
#
# `ships`: True for variants the release matrix builds + publishes. A variant can
# exist here (buildable from the CLI) yet be held out of CI with ships=False.
# This dict is the SINGLE source of truth — generate_firmwares.py projects it to
# docs/install/firmwares.json, which the CI matrix, the ESP Web Tools manifest
# loops, and MoonDeck all read (check_firmwares.py guards the projection).
FIRMWARES: dict[str, dict] = {
    # Default classic ESP32: WiFi AND Ethernet in one binary. The RMII Ethernet
    # driver compiles in (the .eth fragment); whether Eth comes up, and on which
    # pins/PHY, is runtime config (deviceModels.json → NetworkModule → ethInit). A
    # WiFi-only board flashing this just gets WiFi — ethInit() no-ops when no PHY
    # responds, then the WiFi cascade takes over (no GPIO grab, no hang). This
    # replaces the old separate `esp32` (WiFi-only) + `esp32-eth-wifi` keys.
    "esp32": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.eth"],
        "eth_only": False,
        "description": "ESP32 classic — WiFi + Ethernet (RMII; per-board pins/PHY "
                       "from deviceModels.json, default LAN8720 pins).",
        "ships": True,
    },
    "esp32-16mb": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.16mb",
                      "sdkconfig.defaults.eth"],
        "eth_only": False,
        "description": "ESP32 classic with 16 MB flash — WiFi + Ethernet. Same silicon "
                       "as `esp32`; this variant uses the extra flash for bigger OTA "
                       "slots + filesystem (Serg boards, QuinLED Dig-Octa).",
        "ships": True,
    },
    "esp32-eth": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.eth"],
        "eth_only": True,
        "description": "ESP32 classic — Ethernet only (WiFi compiled out; smaller "
                       "image, more RAM). Per-board pins/PHY from deviceModels.json. The "
                       "default `esp32` does WiFi+Ethernet — use this only to drop WiFi.",
        "ships": True,
    },
    "esp32s3-n16r8": {
        "chip": "esp32s3",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s3-n16r8",
                      "sdkconfig.defaults.eth-spi"],
        "eth_only": False,
        "description": "ESP32-S3 DevKitC-1 (N16R8: 16 MB flash, 8 MB octal PSRAM) — WiFi + "
                       "W5500 SPI Ethernet (external module, pins per board in deviceModels.json)",
        "ships": True,
    },
    "esp32s3-n8r8": {
        "chip": "esp32s3",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s3-n8r8",
                      "sdkconfig.defaults.eth-spi"],
        "eth_only": False,
        "description": "ESP32-S3 (N8R8: 8 MB flash, 8 MB octal PSRAM) — WiFi + W5500 SPI "
                       "Ethernet. Half the flash of N16R8; the N16R8 binary overruns an "
                       "8 MB board, so N8R8 boards (LightCrafter etc.) need this variant.",
        "ships": True,
    },
    "esp32p4-eth": {
        "chip": "esp32p4",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32p4-eth"],
        "eth_only": True,
        "description": "Waveshare ESP32-P4-NANO — Ethernet only (IP101 PHY). The "
                       "WiFi-less fallback; esp32p4-eth-wifi adds the C6 radio.",
        "ships": True,
    },
    "esp32p4-eth-wifi": {
        "chip": "esp32p4",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32p4-eth",
                      "sdkconfig.defaults.esp32p4-eth-wifi"],
        "eth_only": False,
        "description": "Waveshare ESP32-P4-NANO — Ethernet + WiFi. WiFi runs on the "
                       "on-board ESP32-C6 over SDIO (esp_wifi_remote + esp_hosted, "
                       "pulled P4-only). First build is longer (managed components).",
        # ships=False: the C6-slave WiFi Kconfig defaults don't survive a plain CI
        # build (no `set-target`), so it isn't reproducible yet — held out of the
        # release matrix. Buildable from the CLI. See backlog § ESP32-P4 round 3.
        "ships": False,
    },
    "esp32s31": {
        "chip": "esp32s31",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s31"],
        "eth_only": False,
        "description": "Espressif ESP32-S31 Function-CoreBoard-1 — WiFi 6 + on-chip "
                       "1 Gbps Ethernet in one image (RISC-V, 16 MB flash, PSRAM). "
                       "esp32s31 is a preview target on the v6.1 IDF line.",
        "ships": True,
    },
}

# IDF target → chip-family label. ONE source for the family vocabulary, shared by:
#   * the ESP Web Tools manifest (`chipFamily`, generate_manifest.py),
#   * the installer's detect-vs-board comparison (deviceModels.json `chip` uses these
#     same strings; install-orchestrator.js normalises detected silicon to them).
# (firmwares.json does NOT store a per-variant family — it's derivable from `chip`;
# see generate_firmwares.py.)
# projectMM aims to support every ESP32-family chip, so new SoCs are added HERE
# once (S2 / C3 / C6 / C5 / H2 / P4 variants) and every consumer follows.
TARGET_TO_FAMILY = {
    "esp32":    "ESP32",
    "esp32s3":  "ESP32-S3",
    "esp32s31": "ESP32-S31",
    "esp32p4":  "ESP32-P4",
}

# Chips IDF still marks "preview" — `idf.py set-target <chip>` refuses without an
# explicit `--preview` flag ("you have to append '--preview' to use any preview
# feature"). Drop a chip from this set once it graduates to a stable target.
PREVIEW_TARGETS = {"esp32s31"}

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


def firmware_cmake_args(firmware: str, release: str = "", version: str = "") -> list[str]:
    """Extra -D cache args for the requested firmware.

    `release` is the release-channel tag (e.g. "latest", "v1.0.0") to burn
    into the binary as MM_RELEASE. Empty for local builds — SystemModule
    then shows the bare semver with no channel suffix.

    `version` overrides MM_VERSION with the pipeline-computed semver
    (compute_version.py): the core for a stable tag, `<core>-dev.<N>` for a
    moving `latest` build. Empty for local builds — build_info.h's #ifndef
    default (library.json) applies.
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
    # Same for the computed version — empty leaves build_info.h's library.json default.
    if version:
        args.append(f'-DMM_VERSION="{version}"')
    if spec["eth_only"]:
        # Drop the WiFi components from the link, and tell our code to compile
        # out the WiFi paths (MM_ETH_ONLY → esp32/main/CMakeLists.txt).
        args.append("-DEXCLUDE_COMPONENTS=" + ";".join(ETH_ONLY_EXCLUDE))
        args.append("-DMM_ETH_ONLY=1")
    # Firmwares that have no Ethernet driver at all (no EMAC sdkconfig and no
    # SPI-PHY sdkconfig) lack the headers platform_esp32.cpp's ethInit() needs,
    # so it won't compile — set MM_NO_ETH and the source provides stubs instead.
    # A variant "has Ethernet" when any sdkconfig fragment enables a PHY driver:
    #   * RMII EMAC (classic/P4): `sdkconfig.defaults.eth` (".eth"), board-specific
    #     ones append "-eth" (e.g. ".esp32p4-eth").
    #   * W5500 SPI (S3, no EMAC): `sdkconfig.defaults.eth-spi` (".eth-spi").
    # Match the "eth" segment in any of these forms so a new eth board (RMII or
    # SPI) doesn't silently stub Ethernet out. The hyphen-suffix forms (`-eth`,
    # `.eth-spi`) are why a bare endswith(".eth") isn't enough.
    has_eth_fragment = any(".eth" in f or f.endswith("-eth")
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


def stale_feature_cache(build_dir: Path, extra: list[str]) -> str | None:
    """Detect a build dir whose cached feature flags disagree with this firmware.

    CMake `-D` flags are written into CMakeCache.txt; *omitting* a flag on a
    later configure does NOT clear it. So if a firmware key's Ethernet-ness
    (MM_NO_ETH / MM_ETH_ONLY) changes while its build dir already exists, the
    stale cache value wins and the binary silently builds for the old feature
    set — e.g. the collapsed `esp32` (WiFi+Eth) reusing a pre-collapse
    WiFi-only dir kept MM_NO_ETH=1 and stubbed Ethernet out (no link, no LED).
    Erasing flash doesn't help: it's a compile-time define, not device state.

    Returns a human-readable reason string when the cache is stale (caller
    should clean + reconfigure), or None when it matches.
    """
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    text = cache.read_text(encoding="utf-8", errors="replace")
    # The feature toggles whose presence/absence changes which code compiles.
    # For each, "wanted" = does this firmware pass the -D, "cached" = is it set
    # in the existing cache. A disagreement means a stale dir.
    for flag in ("MM_NO_ETH", "MM_ETH_ONLY", "MM_NO_WIFI"):
        wanted = any(a.startswith(f"-D{flag}") for a in extra)
        cached = f"{flag}:" in text  # CMake writes `MM_NO_ETH:UNINITIALIZED=1`
        if wanted != cached:
            return (f"{flag} {'set' if cached else 'unset'} in cache but "
                    f"firmware wants it {'set' if wanted else 'unset'}")
    # Value flags (not just present/absent): MM_VERSION / MM_RELEASE carry a string
    # that changes per build. CMake keeps the OLD cached value when the same dir is
    # reused, so a changed --version would silently build the stale version (it's a
    # compile-time define, like the feature flags above). Detect a value mismatch and
    # force a clean reconfigure so the binary never lies about its version.
    for flag in ("MM_VERSION", "MM_RELEASE"):
        wanted = next((a[len(f"-D{flag}="):] for a in extra
                       if a.startswith(f"-D{flag}=")), None)
        if wanted is None:
            continue  # not passed this build — leave the cache alone
        m = re.search(rf"^{flag}:[^=]*=(.*)$", text, re.MULTILINE)
        cached = m.group(1) if m else None
        if cached is not None and cached != wanted:
            return f"{flag} cached as {cached!r} but this build wants {wanted!r}"
    return None


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
    parser.add_argument("--version", default="",
                        help="Override MM_VERSION with the pipeline-computed semver "
                             "(see compute_version.py): core for a stable tag, "
                             "'<core>-dev.<N>' for latest. Omit for local builds "
                             "(library.json applies).")
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
    #
    # KNOWN ISSUE (esp32p4-eth-wifi): esp_wifi_remote's slave target
    # (SLAVE_IDF_TARGET_ESP32C6) is selected by a Kconfig `default ... if
    # IDF_TARGET_ESP32P4` that fires during `set-target` but is dropped by the
    # reconfigure a plain `build` triggers, falling back to ESP32-H2 (no WiFi) and
    # failing on missing CONFIG_WIFI_RMT_* symbols. A clean manual sequence works:
    #   rm -rf build/esp32-esp32p4-eth-wifi && idf.py -B <dir> -DSDKCONFIG=<dir>/sdkconfig \
    #     -DSDKCONFIG_DEFAULTS="..." set-target esp32p4 && (same) build
    # but this wrapper does not yet reproduce it reliably — tracked in
    # docs/backlog/ (ESP32-P4 round 3). Until fixed, build this variant
    # with the manual sequence above.
    extra = firmware_cmake_args(firmware, args.release, args.version)

    # Guard against a build dir configured for a different feature set (a stale
    # MM_NO_ETH / MM_ETH_ONLY in CMakeCache that a plain reconfigure won't clear).
    # Wiping the dir forces the set-target path below, which seeds a clean cache.
    stale = stale_feature_cache(build_dir, extra)
    if stale:
        print(f"Build dir {build_dir.relative_to(ROOT)} has a stale feature "
              f"cache ({stale}); removing it for a clean reconfigure.")
        shutil.rmtree(build_dir)

    if not build_dir.exists():
        print(f"Setting target to {chip} (firmware: {firmware}, build dir: "
              f"{build_dir.relative_to(ROOT)})...")
        # A preview chip (esp32s31 today) needs `--preview` on idf.py itself,
        # before the action, or set-target refuses it.
        preview = ["--preview"] if chip in PREVIEW_TARGETS else []
        r = subprocess.run(cmd + preview + b_arg + extra + ["set-target", chip],
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
