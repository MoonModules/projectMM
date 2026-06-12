# SystemModule

![SystemModule controls](../../assets/screenshots/SystemModule.png)

System-level diagnostics and device identity. Always loaded, always visible in the UI.

## Controls (ordered by change frequency)

**Dynamic (updates every second):**
- `uptime` (read-only, progress) — seconds since boot
- `fps` (read-only) — derived from Scheduler tickTimeUs
- `tickTimeUs` (read-only) — average tick time in microseconds
- `freeHeap` (read-only, progress) — current free heap / total heap
- `freeInternal` (read-only, progress) — current free internal heap / total internal
- `maxBlock` (read-only) — largest contiguous allocatable block

**Configurable:**
- `deviceName` (text, default `MM-XXXX` where XXXX = last 4 hex of MAC) — device name. Used as hostname for mDNS, AP SSID, and UI display. Persisted after item 11.

**Static (set at boot):**
- `version` (read-only) — semver from library.json (`MM_VERSION`), plus the release channel in parentheses when the build was published under one: `1.0.0-rc2 (latest)`, `1.0.0 (v1.0.0)`. The channel (`MM_RELEASE`) is burned in by `release.yml` via `build_esp32.py --release <tag>`; a local / dev build has no channel and shows the bare semver. Semver answers *what code*; the channel answers *which release this device was flashed from* — a moving `latest` build and a tagged release can share a semver but differ in channel. Desktop builds show the bare semver today (the desktop packager doesn't set the channel).
- `build` (read-only) — build date/time
- `firmware` (read-only) — build-time firmware variant key from `src/core/build_info.h` (`MM_FIRMWARE_NAME`): `esp32`, `esp32-eth`, `esp32-eth-wifi`, `esp32s3-n16r8` for the shipped firmware variants; `desktop-macos-arm64` / `desktop-windows-x64` for packaged desktop binaries; `desktop-dev` for unpackaged local desktop builds. Identifies which release asset matches the device — the same key appears in the firmware filenames published by `release.yml`. "Firmware" is the compiled binary; the physical board the firmware runs on lives on the [BoardModule](BoardModule.md) child (code-wired in `main.cpp`, mirrors how Improv sits under Network).
- `chip` (read-only) — chip model (ESP32, ESP32-S3, etc.)
- `sdk` (read-only) — ESP-IDF version string (or compiler on desktop)
- `wifiCoproc` (read-only) — WiFi co-processor firmware status, shown only on boards whose radio is a separate chip (the ESP32-P4 with its on-board ESP32-C6 over esp_hosted). Reports the detected slave firmware version (`C6 fw 2.12.9`) when the link is up, or `not detected` when the C6 never completes its handshake / reports 0.0.0, which is the signature of absent or incompatible C6 slave firmware. Absent on native-radio targets (the platform returns an empty string and the control is not added).
- `flash` (read-only) — total flash chip size
- `firmwarePartition` (read-only, progress) — current app image size / total firmware partition size. Distinct from the `firmware` string control above (which is the build variant identifier); this is how full the partition is. Renamed from the previous shared `firmware` name to avoid the collision that broke `controls.find(c => c.name === "firmware")` callers — see the comment at the binding in [SystemModule.h](../../../src/core/SystemModule.h).
- `psram` (read-only, progress) — used / total PSRAM (only if present)
- `filesystem` (read-only, progress) — used / total filesystem
- `bootReason` (read-only) — human-readable reset reason from `platform::resetReason()` (e.g. `POWERON`, `SW`, `PANIC`, `INT_WDT`, `TASK_WDT`, `BROWNOUT`, `DEEPSLEEP`). Desktop always reports `OK`. The UI flags the reboot button with a red border (`data-crashed="true"`) when the value is one of PANIC / INT_WDT / TASK_WDT / BROWNOUT, indicating the prior boot ended unexpectedly.

On desktop these show "desktop" / "N/A" for hardware-specific fields.

## Device name

`deviceName` is the device's identity across the system: NetworkModule uses it as the mDNS hostname (`deviceName.local`) and the AP SSID, and MoonDeck shows it in the device list. The default `MM-XXXX` derives from the last 4 hex of the MAC.

## Tests

- Unit test: MAC-to-name conversion
- Scenario: verify /api/system returns valid metrics

## Prior art

### projectMM v1

- System info displayed in web UI (heap, FPS, chip info)
- Device name configurable and persisted

### MoonLight

- System diagnostics via REST API
- Device name used for mDNS

## Source

[SystemModule.h](../../../src/core/SystemModule.h)
