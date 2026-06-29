# I2cScanModule

A **core**, domain-neutral diagnostic that scans an I2C bus and reports which device addresses ACK — the standard [`i2cdetect`](https://manpages.debian.org/i2c-tools/i2cdetect.8.en.html) operation, surfaced in the UI. It is the bring-up tool for any I2C peripheral (an audio codec, a sensor, a port expander): set the bus pins, press scan, read off the addresses present. Confirms wiring before a driver tries to talk to the device.

Not auto-wired. Factory-registered like [AudioModule](AudioModule.md), so a board with an I2C bus adds it through `docs/install/deviceModels.json` (its `sda`/`scl` controls carry that board's bus pins) or the user adds it from the UI.

## Controls

- `sda` / `scl` — the bus pins ([Pin](Control.md) controls). Default to **GPIO21/22**, the Arduino-ESP32 core's conventional I2C pair, so the control pre-fills a sensible starting point on a classic ESP32 (the pins route through the GPIO matrix, so they're a convention, not fixed hardware). A board with a fixed bus overrides them in its catalog entry — e.g. the S31's `sda:51, scl:50`.
- `scan` — a **button** (momentary action, [Control.md](Control.md)): pressing it runs the scan now (`onUpdate` → the probe). A button, not a toggle, because it's a one-shot action.
- `result` — a read-only string of the 7-bit addresses found, space-separated hex (e.g. `0x18 0x3c`); empty when none answer.

Scan state ("N devices found", "set sda + scl pins first") reports through the standard [MoonModule](MoonModule.md) `setStatus()` channel.

## How it works

The probe is `platform::i2cScan(sda, scl, out, maxOut)` (declared in `src/platform/platform.h`). That seam is self-contained: it opens a **temporary** I2C master bus on the given pins, probes every 7-bit address (`0x01`–`0x77`), writes the ACKing addresses into the caller's buffer, and tears the bus down. Opening its own short-lived bus (rather than borrowing one) means the scan never conflicts with a bus another driver owns — e.g. the ES8311 codec on the ESP32-S31 holds its own bus in `platform_esp32_es8311.cpp`; the scan probes the same pins independently between codec operations.

On a target without an I2C bus (the inert stub: an I2C-less ESP32, or desktop) the seam returns `kI2cBusUnavailable`, so the scan reports "bus unavailable" rather than a misleading "0 devices found" — the 0 is reserved for a real scan where nothing ACKed.

## Prior art

The bus-scan-as-a-feature mirrors MoonLight's I2C scan diagnostic; the seam name and probe range follow the Linux `i2c-tools` `i2cdetect` convention.

## Source

[I2cScanModule.h](../../../src/core/I2cScanModule.h)
