#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""Collect KPI data for commit descriptions.

Usage:
  uv run scripts/check/collect_kpi.py           # full report
  uv run scripts/check/collect_kpi.py --commit   # commit message format
"""

import argparse
import re
import subprocess
import sys
import os
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ESP32_DIR = ROOT / "esp32"

# Per-host desktop build dir (matches build_desktop.py / package_desktop.py).
# We pick the directory belonging to the OS this script runs on so KPI
# numbers reflect the binary the developer actually has on disk.
import platform as _plat
_HOST = {"Darwin": "macos", "Linux": "linux",
         "Windows": "windows"}.get(_plat.system(), _plat.system().lower())
BUILD_DIR = ROOT / "build" / _HOST

# Hard performance floor for the ESP32 render loop, expressed as an FPS ×
# light-count throughput so it scales to any grid size. Anchored at the 128×128
# reference: 10 FPS × 16384 lights. A smaller grid must run proportionally
# faster (e.g. 64×64 = 4096 lights → 40 FPS floor).
#
# The device reports tick *time* (µs), not FPS — FPS is only ever derived by
# integer division, which loses precision. So the gate compares the measured
# tick_us directly against a per-grid *max tick time*:
#   max_tick_us = lights × 1e6 / MIN_ESP32_FPS_LED_PRODUCT
# Enforced in --commit mode so a regression fails pre-commit.
#
# Anchor: this floor matches what `contract.esp32-eth-wifi.tick_us` promises in
# test/scenarios/light/scenario_GridLayout_grid_sizes.json `size-128x128` —
# 100,000 µs = 10 FPS with the default NoiseEffect workload. The historic 18
# FPS reference (RainbowEffect) is higher because Rainbow is cheaper than Noise;
# reaching 18 FPS on Noise needs algorithmic work on the effect (the Xtensa has
# no FPU, so the simplex float math dominates).
MIN_ESP32_FPS_LED_PRODUCT = 10 * 16384  # 163840

sys.path.insert(0, str(ROOT / "scripts" / "build"))

def run(cmd, cwd=None, timeout=30):
    # encoding="utf-8" + errors="replace" — subprocess defaults to the locale
    # codec when text=True, which is cp1252 on Windows and crashes on bytes
    # >= 0x80 from tools like idf.py size that emit utf-8 (deg/box-drawing).
    r = subprocess.run(cmd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace",
                       cwd=cwd, timeout=timeout)
    return r.stdout + r.stderr, r.returncode

# ---------------------------------------------------------------------------
# Collectors — return dicts of KPI data
# ---------------------------------------------------------------------------

def _pick_first_existing(*paths):
    """Return the first Path that exists, or None."""
    for p in paths:
        if p.exists():
            return p
    return None


def collect_desktop():
    kpi = {}

    # Binary names + locations differ per host: macOS/Linux drop the
    # executable under <build>/, MSVC multi-config under <build>/Release/.
    # Cover both forms so KPI on Windows doesn't silently report nothing.
    # Same fallback shape test_desktop.py uses.
    projectMM = _pick_first_existing(
        BUILD_DIR / "projectMM",
        BUILD_DIR / "projectMM.exe",
        BUILD_DIR / "Release" / "projectMM.exe",
    )
    if projectMM:
        kpi["binary_kb"] = projectMM.stat().st_size // 1024

    test_exe = _pick_first_existing(
        BUILD_DIR / "test" / "mm_tests",
        BUILD_DIR / "test" / "mm_tests.exe",
        BUILD_DIR / "test" / "Release" / "mm_tests.exe",
    )
    if test_exe:
        out, rc = run([str(test_exe)], cwd=BUILD_DIR)
        for line in out.splitlines():
            if "test cases:" in line:
                kpi["tests"] = line.strip()
                break

    scenarios = _pick_first_existing(
        BUILD_DIR / "test" / "mm_scenarios",
        BUILD_DIR / "test" / "mm_scenarios.exe",
        BUILD_DIR / "test" / "Release" / "mm_scenarios.exe",
    )
    if scenarios:
        out, rc = run([str(scenarios)], cwd=ROOT)
        # One tick per scenario — the slowest MEASURE step (the contract-
        # relevant worst-case timing). The KPI one-liner becomes a
        # 10-number-ish series that maps 1:1 to the scenario list, so a
        # regression in any scenario shows up as a single bumped digit.
        # `=== Scenario: name ===` delimits each scenario block; MEASURE
        # lines inside it carry "tick=Xus".
        per_scenario_max: list[int] = []
        current_max = 0
        buffer_lights = []
        def _flush():
            nonlocal current_max
            if current_max > 0:
                per_scenario_max.append(current_max)
            current_max = 0
        for line in out.splitlines():
            if line.startswith("=== Scenario:"):
                _flush()
                continue
            if "MEASURE" in line:
                m = re.search(r'tick=(\d+)us', line)
                if m:
                    t = int(m.group(1))
                    if t > current_max:
                        current_max = t
            if "Buffer:" in line and "lights" in line:
                parts = line.strip().split()
                for i, p in enumerate(parts):
                    if p == "Buffer:" and i + 1 < len(parts):
                        try:
                            buffer_lights.append(int(parts[i + 1]))
                        except ValueError:
                            pass
            if "scenario(s)" in line:
                kpi["scenarios"] = line.strip()
        _flush()
        if per_scenario_max:
            kpi["tick_us"] = per_scenario_max
            kpi["fps"] = [1000000 // t if t > 0 else 0 for t in per_scenario_max]
        if buffer_lights:
            kpi["lights"] = max(buffer_lights)

    check = ROOT / "scripts" / "check" / "check_platform_boundary.py"
    if check.exists():
        _, rc = run([sys.executable, str(check)])
        kpi["boundary"] = "PASS" if rc == 0 else "FAIL"

    specs = ROOT / "scripts" / "check" / "check_specs.py"
    if specs.exists():
        out, rc = run([sys.executable, str(specs)])
        kpi["specs_check"] = "PASS" if rc == 0 else "FAIL"
        for line in out.splitlines():
            if "Spec check:" in line:
                kpi["specs_summary"] = line.strip()

    return kpi

def collect_esp32():
    kpi = {}
    # Per-firmware build dirs under build/esp32-*/ (plan-19.1). Pick the dir
    # whose projectMM.bin was written most recently — that's the binary
    # the developer most recently rebuilt and would consider the current
    # KPI source. Sort by the firmware mtime, not the dir mtime, because
    # a sdkconfig save or stray touch can bump the dir mtime without a
    # rebuild — picking by dir mtime would surface stale binaries.
    candidates = [p for p in (ROOT / "build").glob("esp32-*")
                  if (p / "projectMM.bin").exists()]
    candidates.sort(key=lambda p: (p / "projectMM.bin").stat().st_mtime,
                    reverse=True)
    if not candidates:
        return kpi
    esp32_build = candidates[0]
    # Record which firmware variant the numbers came from so a developer
    # with multiple build dirs can see whether the KPI reflects what they
    # think it does. See docs/architecture.md § Firmware vs board.
    kpi["firmware"] = esp32_build.name[len("esp32-"):]

    try:
        from build_esp32 import find_idf, idf_env, idf_cmd
        idf_path = find_idf()
        if idf_path:
            env = idf_env(idf_path)
            cmd = idf_cmd(idf_path)
            # -B + -DSDKCONFIG mirror build_esp32.py so idf.py reads the
            # per-firmware sdkconfig (the build dir's own copy), not the
            # project-root one which may belong to a different firmware.
            r = subprocess.run(
                cmd + ["-B", str(esp32_build),
                       "-DSDKCONFIG=" + str(esp32_build / "sdkconfig"),
                       "size"],
                capture_output=True, text=True,
                encoding="utf-8", errors="replace",
                cwd=ESP32_DIR, env=env, timeout=60)
            out = r.stdout + r.stderr

            flash_total = 0
            for line in out.splitlines():
                parts = [p.strip() for p in line.split("│") if p.strip()]
                if len(parts) >= 2:
                    name = parts[0]
                    try: used = int(parts[1])
                    except ValueError: continue
                    if "Flash Code" in name or "Flash Data" in name:
                        flash_total += used
                    if name == "DRAM" and len(parts) >= 4:
                        kpi["dram_used"] = used
                        try: kpi["dram_free"] = int(parts[3])
                        except ValueError: pass
                if "app partition" in line and "free" in line:
                    m = re.search(r'\((\d+)%\)\s+free', line)
                    if m:
                        kpi["partition_free_pct"] = int(m.group(1))
                if "Total image size" in line:
                    m = re.search(r'(\d+)\s+bytes', line)
                    if m:
                        kpi["image_bytes"] = int(m.group(1))
            if flash_total:
                kpi["flash_kb"] = flash_total // 1024
    except Exception:
        pass

    # Read tick/FPS from monitor.log. Do a live capture against the canonical
    # port (scripts/moondeck.json) when the log is stale OR when it exists but
    # has no parseable tick line — instead of silently skipping ESP32 KPI.
    log = ESP32_DIR / "monitor.log"
    stale = (not log.exists()) or (time.time() - log.stat().st_mtime) > 300

    # Only extract from a log we trust: a non-stale file, or one a live capture
    # just refreshed. If the file is stale and the capture fails (port absent,
    # garbled output), do NOT fall back to the old file — a stale tick reported
    # as fresh is worse than no ESP32 KPI at all.
    if stale:
        if _live_capture(log):
            _extract_esp32_tick(log, kpi)
    else:
        if not _extract_esp32_tick(log, kpi):
            # File is fresh but unparseable (no tick line) — one capture retry.
            if _live_capture(log):
                _extract_esp32_tick(log, kpi)

    return kpi

def _extract_esp32_tick(log, kpi):
    if not log.exists():
        return False
    for line in reversed(log.read_text(encoding="utf-8", errors="replace").splitlines()):
        if "tick:" not in line:
            continue
        # Format: "tick: 108us (FPS: 9259)  free: 215180  maxBlock: 63488"
        m = re.search(r'tick:\s*(\d+)us', line)
        if m:
            tick_us = int(m.group(1))
            kpi["tick_us"] = tick_us
            kpi["fps"] = 1000000 // tick_us if tick_us > 0 else 0
        m = re.search(r'free:\s*(\d+)', line)
        if m:
            kpi["heap_free"] = int(m.group(1))
        return "tick_us" in kpi
    return False

def _live_capture(log, seconds=15):
    """Capture ESP32 serial output to monitor.log for ~seconds. Returns True on success."""
    import json
    cfg = ROOT / "scripts" / "moondeck.json"
    if not cfg.exists():
        return False
    try:
        state = json.loads(cfg.read_text(encoding="utf-8"))
        # Port lives inside the active network record (post-networks refactor
        # in moondeck.py). Fall back to the legacy top-level `port` so an
        # un-migrated moondeck.json still works.
        active_name = state.get("active_network")
        active = next((n for n in (state.get("networks") or [])
                       if n.get("name") == active_name), None)
        port = (active or {}).get("port") or state.get("port")
    except Exception:
        return False
    if not port or not Path(port).exists():
        print(f"  ESP32 KPI: configured port {port} not present, skipping live capture")
        return False
    try:
        import serial
    except ImportError:
        return False
    print(f"  ESP32 KPI: capturing {seconds}s from {port}...")
    try:
        ser = serial.Serial(port, 115200, timeout=1)
    except Exception as e:
        print(f"  ESP32 KPI: cannot open {port}: {e}")
        return False
    end = time.time() + seconds
    try:
        with open(log, "w") as f:
            while time.time() < end:
                line = ser.readline().decode("utf-8", errors="replace").rstrip("\r\n")
                if line:
                    f.write(line + "\n")
                    f.flush()
    finally:
        ser.close()
    return True

def collect_code():
    kpi = {}
    src_dir = ROOT / "src"
    test_dir = ROOT / "test"

    src_files = list(src_dir.rglob("*.h")) + list(src_dir.rglob("*.cpp"))
    kpi["src_files"] = len(src_files)
    # encoding="utf-8" — sources contain non-ASCII (→, µ, ×) in comments.
    # errors="replace" so any garbled file in the build dir doesn't crash KPI.
    kpi["src_lines"] = sum(f.read_text(encoding="utf-8", errors="replace").count("\n") for f in src_files)

    test_files = [f for f in test_dir.rglob("*.cpp") if f.name != "doctest.h"]
    kpi["test_files"] = len(test_files)
    kpi["test_lines"] = sum(f.read_text(encoding="utf-8", errors="replace").count("\n") for f in test_files)

    kpi["specs"] = len(list((ROOT / "docs" / "moonmodules").rglob("*.md")))
    kpi["scenarios"] = len(list((ROOT / "test" / "scenarios").rglob("*.json")))

    # Lizard
    out, _ = run(
        ["uv", "run", "--with", "lizard", "python3", "-m", "lizard",
         "src/", "-l", "cpp", "-T", "nloc=60", "-T", "cyclomatic_complexity=10",
         "-x", "src/ui/*"],
        cwd=ROOT, timeout=30
    )
    warnings = []
    in_warnings = False
    for line in out.splitlines():
        if "Warning" in line and "!!!!" in line:
            in_warnings = True
            continue
        if in_warnings and "---" in line:
            continue
        if in_warnings and line.strip() and "NLOC" not in line and "Total" not in line and "====" not in line:
            warnings.append(line.strip())
        if in_warnings and "Total" in line:
            in_warnings = False
    kpi["lizard_warnings"] = len(warnings)
    kpi["lizard_details"] = warnings

    return kpi

# ---------------------------------------------------------------------------
# Formatters
# ---------------------------------------------------------------------------

def format_oneliner(desktop, esp32, code):
    parts = []
    if "lights" in desktop:
        parts.append(f"{desktop['lights']}lights")
    if "binary_kb" in desktop:
        parts.append(f"PC:{desktop['binary_kb']}KB")
    if "tick_us" in desktop:
        ticks = '/'.join(str(t) for t in desktop['tick_us'])
        fps = '/'.join(str(f) for f in desktop.get('fps', []))
        parts.append(f"tick:{ticks}us(FPS:{fps})")
    if "flash_kb" in esp32:
        parts.append(f"ESP32:{esp32['flash_kb']}KB")
    if "tick_us" in esp32:
        parts.append(f"tick:{esp32['tick_us']}us(FPS:{esp32.get('fps', '?')})")
    if "heap_free" in esp32:
        parts.append(f"heap:{esp32['heap_free']//1024}KB")
    parts.append(f"src:{code['src_files']}({code['src_lines']})")
    parts.append(f"test:{code['test_files']}({code['test_lines']})")
    if code["lizard_warnings"] > 0:
        parts.append(f"lizard:{code['lizard_warnings']}w")
    return "KPI: " + " | ".join(parts)

def format_full(desktop, esp32, code):
    lines = []
    lines.append("KPI Details:")

    lines.append("  Desktop:")
    if "lights" in desktop:
        lines.append(f"    Lights: {desktop['lights']:,}")
    if "binary_kb" in desktop:
        lines.append(f"    Binary: {desktop['binary_kb']} KB")
    if "tests" in desktop:
        lines.append(f"    {desktop['tests']}")
    if "tick_us" in desktop:
        ticks = ', '.join(f"{t}us" for t in desktop['tick_us'])
        fps = ', '.join(str(f) for f in desktop.get('fps', []))
        lines.append(f"    tick: {ticks} (FPS: {fps}) (per scenario)")
    if "scenarios" in desktop:
        lines.append(f"    {desktop['scenarios']}")
    if "boundary" in desktop:
        lines.append(f"    Platform boundary: {desktop['boundary']}")
    if "specs_check" in desktop:
        lines.append(f"    Specs: {desktop.get('specs_summary', desktop['specs_check'])}")

    if esp32:
        lines.append("  ESP32:")
        if "image_bytes" in esp32:
            lines.append(f"    Image: {esp32['image_bytes']:,} bytes ({esp32.get('partition_free_pct', '?')}% partition free)")
        if "flash_kb" in esp32:
            lines.append(f"    Flash (code+data): {esp32['flash_kb']} KB")
        if "dram_used" in esp32:
            total = esp32["dram_used"] + esp32.get("dram_free", 0)
            lines.append(f"    DRAM: {esp32['dram_used']:,} / {total:,} ({esp32.get('dram_free', 0):,} free)")
        if "tick_us" in esp32:
            lines.append(f"    tick: {esp32['tick_us']}us (FPS: {esp32.get('fps', '?')})  heap free: {esp32.get('heap_free', '?')}")

    lines.append("  Code:")
    lines.append(f"    {code['src_files']} source files ({code['src_lines']} lines)")
    lines.append(f"    {code['test_files']} test files ({code['test_lines']} lines)")
    lines.append(f"    {code['specs']} specs, {code['scenarios']} scenarios")
    lines.append(f"    Lizard: {code['lizard_warnings']} warnings")
    for w in code.get("lizard_details", []):
        lines.append(f"      {w}")

    return "\n".join(lines)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--commit", action="store_true",
                        help="Output in commit message format (one-liner + details)")
    args = parser.parse_args()

    desktop = collect_desktop()
    esp32 = collect_esp32()
    code = collect_code()

    if args.commit:
        print(format_oneliner(desktop, esp32, code))
        print("(see KPI Details at bottom)")
        print()
        print(format_full(desktop, esp32, code))
    else:
        # Full interactive report
        print("=" * 50)
        print("  projectMM KPI Report")
        print("=" * 50)
        print()
        print(format_oneliner(desktop, esp32, code))
        print()
        print(format_full(desktop, esp32, code))
        print()
        print("=" * 50)

    # Hard throughput gate: if a live ESP32 tick was captured, it must clear the
    # floor. The device reports tick *time* (µs), so compare that directly — no
    # lossy FPS division. The per-grid budget scales with light count:
    #   max_tick_us = lights × 1e6 / MIN_ESP32_FPS_LED_PRODUCT
    # so a smaller grid gets proportionally less time (10 FPS at 128×128).
    # An absent ESP32 reading is not a failure — the caller decides whether a
    # missing measurement is acceptable (see CLAUDE.md Lifecycle Events,
    # Event 1 gate 7 — KPI collection).
    # Only --commit mode aborts on a breach; a plain interactive report just
    # warns, so viewing KPIs on an unlucky slow sample does not exit non-zero.
    esp32_tick = esp32.get("tick_us")
    lights = desktop.get("lights")
    if esp32_tick is not None and lights:
        max_tick = round(lights * 1_000_000 / MIN_ESP32_FPS_LED_PRODUCT)
        if esp32_tick > max_tick:
            print()
            label = "FAIL" if args.commit else "WARN"
            floor_fps = MIN_ESP32_FPS_LED_PRODUCT // 16384
            print(f"{label}: ESP32 render tick {esp32_tick}us exceeds the {max_tick}us "
                  f"budget for {lights} lights ({floor_fps} FPS at the 128×128 / 16384-light "
                  f"reference).")
            if args.commit:
                sys.exit(1)

if __name__ == "__main__":
    main()
