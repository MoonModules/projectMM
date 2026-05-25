# MoonDeck Script Reference

## UI Features

- **Status dots** on each card: grey (not run), orange (running), green (exit 0), red (exit non-zero).
- **Run/Stop toggle** for long-running scripts (Run desktop, Monitor ESP32).
- **Group headers** in the sidebar (build, test, run, check, scenario, setup).
- **Tab persistence** — selected tab survives page refresh.
- **Process detection** — on page load, checks if projectMM or idf.py is already running and shows Stop button.

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

Launch the desktop executable as a detached background process and exit. The app keeps running across other MoonDeck scripts and outlives MoonDeck itself — the same model as flashing an ESP32, where the device runs independently of this console.

```bash
uv run scripts/run/run_desktop.py
```

Re-running is idempotent: any existing `projectMM` instance is stopped first, then a fresh one is launched. Output goes to `build/projectMM.log`. Build first.

While the app is running, MoonDeck shows the button as **Stop** (a 5-second poll on `/api/running` detects the live process via `process_name`). Pressing Stop terminates the app; pressing Run again restarts it. From the CLI: `pkill -f build/projectMM`.

### preview_installer

Locally preview the web installer page at <https://ewowi.github.io/projectMM/install/> without tagging a release. Stages `docs/install/index.html` + `src/ui/release-picker.js` into `build/install-preview/` and serves them via Python's `http.server` on port 8000.

```bash
uv run scripts/run/preview_installer.py
# open http://localhost:8000/ in Chrome / Edge / Opera
```

Long-running — MoonDeck shows **Stop** while the server is up. This is "Recipe A" from [docs/install/README.md](../docs/install/README.md): the picker populates against the real GitHub Releases API and dropdowns work, but clicking **Install** fails because the local server has no `releases/` tree. Useful for iterating on HTML/CSS/JS. Add `?nocache=1` to the URL to bypass the picker's 5-minute sessionStorage cache while editing.

For an end-to-end preview that can actually flash (Recipe B), follow the script in [docs/install/README.md](../docs/install/README.md) — that flow pulls a CI build's artifacts and is too stateful for a one-click button.

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

Scenarios are JSON files in `test/scenarios/`.

## Live Tab

### live_scenario

Run scenario tests against a live running device via HTTP.

```bash
uv run scripts/scenario/run_live_scenario.py                                    # all scenarios vs localhost:8080
uv run scripts/scenario/run_live_scenario.py --host 192.168.1.210               # vs ESP32
uv run scripts/scenario/run_live_scenario.py --name control-change              # one scenario
uv run scripts/scenario/run_live_scenario.py --update-baseline                  # save baseline
uv run scripts/scenario/run_live_scenario.py --compare-baseline                 # detect regressions
```

Executes scenario steps (add_module, set_control, delete_module) via REST API. Collects per-step FPS and heap measurements. Compares against stored baselines to detect performance regressions.

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

Build one of the shipping ESP32 board variants. Four MoonDeck buttons map to four `--board` values:

| Button | `--board` | Chip | What's in the image |
|---|---|---|---|
| **Build esp32** | `esp32` | `esp32` | WiFi only. No Eth pins reserved. |
| **Build esp32-eth** | `esp32-eth` | `esp32` | Ethernet only (WiFi compiled out → smaller image, more free RAM). Olimex ESP32-Gateway pin defaults (LAN8720 @ MDIO 0, PHY RST GPIO 5). |
| **Build esp32-eth-wifi** | `esp32-eth-wifi` | `esp32` | Ethernet + WiFi both available. Olimex pin defaults. |
| **Build esp32s3-n16r8** | `esp32s3-n16r8` | `esp32s3` | ESP32-S3 DevKitC-1 with the N16R8 module (16 MB flash, 8 MB octal PSRAM). WiFi only. |

```bash
uv run scripts/build/build_esp32.py --board esp32
uv run scripts/build/build_esp32.py --board esp32-eth
uv run scripts/build/build_esp32.py --board esp32-eth-wifi
uv run scripts/build/build_esp32.py --board esp32s3-n16r8
```

Auto-detects ESP-IDF installation, sets target if needed, builds, and shows flash/RAM usage summary. Switching boards cleans `build/` automatically (recorded via `build/.mm_board`).

Eth pin map is currently baked in at build time. The `esp32-eth` and `esp32-eth-wifi` builds were verified on the [Olimex ESP32-Gateway](https://www.olimex.com/Products/IoT/ESP32/ESP32-GATEWAY/open-source-hardware) (LAN8720 PHY, reset on GPIO 5, MDIO addr 0). Boards with the same PHY but different pins (e.g. WT32-ETH01: reset on GPIO 16) need a local rebuild today; runtime PHY/pin selection is on the 2.0 roadmap.

Each ESP32-S3 SKU has its own build key because the sdkconfig fragment encodes flash size, partition layout, and PSRAM mode — flashing an `n16r8` binary onto a different module (e.g. N8R2) misaligns the partition table or fails PSRAM init. New SKUs become new keys (e.g. `esp32s3-n8r8`); we don't ship a generic `esp32s3` shortcut.

`--profile` is deprecated and accepted one release for migration: `--profile default` → `--board esp32`, `--profile eth-only` → `--board esp32-eth`. The legacy `build_esp32_ethonly.py` wrapper still works (it now forwards `--board esp32-eth`).

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

### improv_provision

Push WiFi credentials to a running projectMM device over USB-serial. Uses the [Improv-WiFi](https://www.improv-wifi.com/serial/) protocol — the same wire format the browser flow at improv-wifi.com uses. Device must be running a firmware that includes the Improv listener.

**One-click flow**: pick the device's port in MoonDeck, hit **Improv WiFi**. The script auto-detects the host machine's currently-joined WiFi (SSID + password via macOS Keychain / Linux NetworkManager / Windows `netsh`) and sends it to the device. The device replies with its new URL when STA comes up — typically 5-10 s end to end.

```bash
# Use host's currently-joined WiFi (one click in MoonDeck → equivalent CLI):
uv run scripts/build/improv_provision.py --port /dev/tty.usbserial-XXXX

# Override SSID + password (rack / CI / different network):
uv run scripts/build/improv_provision.py \
  --port /dev/tty.usbserial-XXXX \
  --ssid "MyWiFi" \
  --password "hunter2"

# Self-test the framing — no serial port needed (CI / pre-commit):
uv run scripts/build/improv_provision.py --self-test
```

Exits 0 with `==> provisioned: http://<ip>/` on success. On a USB hub, shell-loop over the ports:

```bash
for port in /dev/tty.usbserial-*; do
  uv run scripts/build/improv_provision.py --port "$port"
done
```

The host-WiFi reader lives at [scripts/build/host_wifi.py](build/host_wifi.py) and runs standalone for diagnosis (`python3 scripts/build/host_wifi.py` prints the detected SSID + password). The first macOS run pops a Keychain access dialog — the OS doing its job; we don't try to bypass it.

Replaces v1's `deploy/wifi.py` + `deploy/flashfs.py --wifi` partition-baking flow — the device stays running, no flash mode required. Full module + protocol details: [docs/moonmodules/core/ImprovProvisioningModule.md](../docs/moonmodules/core/ImprovProvisioningModule.md).
