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
- `version` (read-only) — projectMM version from library.json
- `build` (read-only) — build date/time
- `firmware` (read-only) — build-time firmware variant key from `src/core/build_info.h` (`MM_FIRMWARE_NAME`): `esp32`, `esp32-eth`, `esp32-eth-wifi`, `esp32s3-n16r8` for the shipped firmware variants; `desktop-macos-arm64` / `desktop-windows-x64` for packaged desktop binaries; `desktop-dev` for unpackaged local desktop builds. Identifies which release asset matches the device — the same key appears in the firmware filenames published by `release.yml`. "Firmware" is the compiled binary; the physical board is a separate concept ([architecture.md § Firmware vs board](../../architecture.md#firmware-vs-board)).
- `chip` (read-only) — chip model (ESP32, ESP32-S3, etc.)
- `sdk` (read-only) — ESP-IDF version string (or compiler on desktop)
- `flash` (read-only) — total flash chip size
- `psram` (read-only, progress) — used / total PSRAM (only if present)
- `filesystem` (read-only, progress) — used / total filesystem
- `bootReason` (read-only) — human-readable reset reason from `platform::resetReason()` (e.g. `POWERON`, `SW`, `PANIC`, `INT_WDT`, `TASK_WDT`, `BROWNOUT`, `DEEPSLEEP`). Desktop always reports `OK`. The UI flags the reboot button with a red border (`data-crashed="true"`) when the value is one of PANIC / INT_WDT / TASK_WDT / BROWNOUT, indicating the prior boot ended unexpectedly.

On desktop these show "desktop" / "N/A" for hardware-specific fields.

## Progress bar controls

Sized controls (heap, flash, psram, filesystem) show as progress bars in the UI: current value relative to total capacity. This gives an immediate visual sense of how full each resource is. The UI auto-renders these based on a `progress` control type (min=0, max=total, value=used or free).

## Device name

Default: `MM-XXXX` where XXXX = last 4 hex characters of the device's MAC address (e.g. `MM-3A7F`). Used for:
- **mDNS**: NetworkModule registers `name.local`
- **AP SSID**: NetworkModule uses name as the AP network name
- **Device identification**: MoonDeck shows name in the device list

The device name is the device's identity across the system. Used consistently as `deviceName` in code, UI, and docs. Internally used as the hostname for mDNS registration (`deviceName.local`).

## Lifecycle

- `setup()` — read MAC address, compute default name, read chip info
- `loop1s()` — update dynamic controls (heap, fps, uptime)
- No hot-path work — all updates in loop1s

## Platform

Needs:
- `platform::getMacAddress(uint8_t[6])` — for default name generation
- `esp_chip_info()` — chip model/revision (ESP32 only)
- `esp_idf_version_get()` — IDF version string (ESP32 only)
- `esp_partition` APIs — flash/firmware size (ESP32 only)

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
