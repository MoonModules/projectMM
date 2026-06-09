# RMT LED Driver

Output driver for WS2812B-class addressable LEDs over the ESP32 **RMT** peripheral, one strand on one GPIO. Reads the Drivers container's buffer, applies the shared [Correction](Correction.md) (brightness / channel order / RGBW white) per light, and emits the WS2812 1-wire signal. Classic ESP32 only this increment — on the ESP32-S3 and desktop the RMT platform seam is a no-op and the driver is inert.

## Wire contract — WS2812B

1-wire NRZ at 800 kHz, no clock line. Each data bit is a 1.25 µs cell that starts HIGH then drops LOW; the HIGH duration encodes the bit:

| | HIGH | period | meaning |
|---|---|---|---|
| `0` bit | 350 ns | 1250 ns | short high, long low |
| `1` bit | 700 ns | 1250 ns | long high, short low |

Bits are sent **MSB-first** within each byte; channel order (GRB, GRBW, …) is the light preset applied by `Correction` before the encode, so the encoder itself is order-agnostic. Frames are latched by ≥ 300 µs idle-LOW (current WS2812B/SK6812 silicon — the old 50 µs value is dead). These timings live in `LedDriverConfig` and are converted to RMT ticks from the peripheral's granted resolution (≈ 40 MHz / 25 ns per tick), so they are not hard-coded to one clock.

## Synchronous show (blocks the render tick)

`loop()` encodes the frame and calls `platform::rmtWs2812Show()` synchronously — it returns after the DMA completes and the reset gap is held. For one 100-pixel strand that is ~3 ms charged to the render tick, which is acceptable for a single strand. A dedicated core-1 driver task (the WiFi-glitch mitigation from the [LED driver analysis](../../../backlog/leddriver-analysis-top-down.md) § 7.2) and per-module core affinity are a later increment; until then, large strands or WiFi-interrupt-sensitive installs may show timing artifacts. See the [increment-1 plan](../../../backlog/leddriver-increment-1-plan.md).

## Controls

- `gpio` (uint8_t, default 18, range 0–48) — the data pin. Changing it re-initialises the RMT channel live (no reboot needed). The default avoids GPIO 4/5, which the on-device loopback test uses.

## Cross-domain wiring

The driver is added as a child of the `Drivers` container in `main.cpp` (under `if constexpr (platform::isEsp32)`), exactly like [ArtNetSendDriver](ArtNetSendDriver.md): it receives `setSourceBuffer` / `setCorrection` / `setLayer` from `Drivers::passBufferToDrivers`, and applies the same `const Correction*` ArtNet uses. The **symbol encode** (`encodeWs2812Symbols` in `RmtSymbol.h`) is domain code in `src/light/` so it is host-testable; the **peripheral** (`platform::rmtWs2812*` in `src/platform/esp32/platform_esp32_rmt.cpp`) is the only ESP-IDF-touching part.

## Tests

- **Encoder (CI, host):** `test/unit/light/unit_RmtLedEncoder.cpp` asserts the bit→symbol contract (MSB-first, exact T0H/T1H tick widths, GRB ordering via Correction, RGBW → 32 symbols/light) with no hardware — the same test was written red before the encoder and pins it now.
- **HAL loopback (on device):** the RMT peripheral is a transceiver — `test/device/device_RmtLoopback.cpp` jumpers the TX pin to an RX pin, captures the emitted pulses, decodes them back to bytes, and asserts they equal what was sent. Independent proof that a GPIO emits correct WS2812 data on real silicon.

## Prior art

The WS2812 protocol fundamentals and the RMT-first / loopback-test strategy come from the project's [LED driver analysis](../../../backlog/leddriver-analysis-top-down.md), which studies FastLED's `clockless_rmt_esp32`, hpwit's I2S drivers, and WLED — read for the lessons, not copied. RMT-v1-style manual ping-pong is what makes RMT more WiFi-resilient than the DMA-less v2 default ([FastLED #2082](https://github.com/FastLED/FastLED/issues/2082)).

## Source

[RmtLedDriver.h](../../../../src/light/drivers/RmtLedDriver.h)
