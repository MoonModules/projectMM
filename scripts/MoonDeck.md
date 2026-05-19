# MoonDeck Script Reference

## UI Features

- **Status dots** on each card: grey (not run), orange (running), green (exit 0), red (exit non-zero).
- **Run/Stop toggle** for long-running scripts (Run desktop, Monitor ESP32).
- **Group headers** in the sidebar (build, test, run, check, scenario, setup).
- **Tab persistence** — selected tab survives page refresh.
- **Process detection** — on page load, checks if mmv3 or idf.py is already running and shows Stop button.

## PC Tab

### build_desktop

Build the desktop target using CMake.

```bash
uv run scripts/build/build_desktop.py
```

Runs `cmake -B build -DCMAKE_BUILD_TYPE=Release` then `cmake --build build`.

### test_desktop

Run the desktop test suite.

```bash
uv run scripts/test/test_desktop.py
```

Runs `./build/test/mm_tests -s` (doctest with all test cases shown).

### run_desktop

Run the desktop executable. Long-running — shows Stop button.

```bash
uv run scripts/run/run_desktop.py
```

Kills any already-running mmv3, then runs `./build/mmv3`. Build first.

### check_platform_boundary

Verify that platform-specific code stays inside `src/platform/`.

```bash
uv run scripts/check/check_platform_boundary.py
```

Scans all source files outside `src/platform/` for forbidden includes and platform `#ifdef`s.

### scenario_pipeline

Run scenario tests. Replays JSON scenario files in-process.

```bash
uv run scripts/scenario/run_scenario.py                       # run all
uv run scripts/scenario/run_scenario.py --name base-pipeline   # run one
```

Scenarios are JSON files in `test/scenarios/`. When HTTP API is added, the same JSON files will work against a live system.

## ESP32 Tab

### setup_esp_idf

Set up ESP-IDF Python environment.

```bash
uv run scripts/build/setup_esp_idf.py
```

Finds the ESP-IDF installation and runs `install.sh` to create the Python venv. Run once after installing ESP-IDF or after a Python version change.

### clean_esp32

Clean the ESP32 build directory.

```bash
uv run scripts/build/clean_esp32.py
```

Removes `esp32/build/` and `esp32/sdkconfig`. Run after ESP-IDF updates, Python version changes, or chip target changes.

### build_esp32

Build for an ESP32 chip target.

```bash
uv run scripts/build/build_esp32.py --env esp32
```

Auto-detects ESP-IDF installation, sets target if needed, builds, and shows flash/RAM usage summary.

### flash_esp32

Flash firmware to an ESP32 device.

```bash
uv run scripts/build/flash_esp32.py --port /dev/tty.usbserial-0001
```

### monitor_esp32

Monitor serial output. Long-running — shows Stop button.

```bash
uv run scripts/run/monitor_esp32.py --port /dev/tty.usbserial-0001
```

Reads serial at 115200 baud. Output streams to MoonDeck's log and is saved to `esp32/monitor.log` for later inspection (useful when crashes flood the output).
