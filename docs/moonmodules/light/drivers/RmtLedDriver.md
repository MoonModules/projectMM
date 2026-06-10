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

- `gpio` (uint8_t, default 18, range 0–48) — the data / TX pin. Changing it re-initialises the RMT channel live (no reboot needed). The loopback self-test also transmits on this pin, so it validates the actual output.
- `loopbackRxPin` (uint8_t, default 5, range 0–48) — the RX pin for the loopback self-test. Jumper it to `gpio` to run the test.
- `loopbackTest` (bool) — tick to run a one-shot RMT TX→RX loopback self-test (see Self-test below). Auto-resets after running; the result lands in the module's status field.

## Cross-domain wiring

The driver is added as a child of the `Drivers` container in `main.cpp` (under `if constexpr (platform::isEsp32)`), exactly like [ArtNetSendDriver](ArtNetSendDriver.md): it receives `setSourceBuffer` / `setCorrection` / `setLayer` from `Drivers::passBufferToDrivers`, and applies the same `const Correction*` ArtNet uses. The **symbol encode** (`encodeWs2812Symbols` in `RmtSymbol.h`) is domain code in `src/light/` so it is host-testable; the **peripheral** (`platform::rmtWs2812*` in `src/platform/esp32/platform_esp32_rmt.cpp`) is the only ESP-IDF-touching part.

## Loopback self-test (on device)

The RMT peripheral is a transceiver, so the driver can verify its own output on real silicon — no separate test firmware. Jumper `gpio` (TX) to `loopbackRxPin`, then tick the `loopbackTest` control: the driver transmits a known WS2812 pattern out the data pin, captures it back on the RX pin, decodes, and compares. The outcome goes to the module's **status field** (`setStatus`): `loopback PASS`, `loopback FAIL: sent … got …`, or `loopback: jumper not detected` (a plain-GPIO continuity pre-check runs first, so a wiring fault is reported as such, not mistaken for a code bug). The test reconfigures the data pin and briefly drives the test pattern, so any strip on `gpio` flickers once during the run; normal output resumes after. All hardware lives in `platform::rmtWs2812Loopback`.

## Tests

- **Encoder (CI, host):** `test/unit/light/unit_RmtLedEncoder.cpp` asserts the bit→symbol contract (MSB-first, exact T0H/T1H tick widths, GRB ordering via Correction, RGBW → 32 symbols/light) with no hardware — written red before the encoder, pins it now.
- **Lifecycle (CI, host):** `test/unit/light/unit_RmtLedDriver_lifecycle.cpp` pins the symbol-buffer ownership — sized in `onBuildState`, survives a rebuild (reinit must not free it), freed on teardown — the class of bug that once reached hardware, now caught on every push.

## Prior art

The WS2812 protocol fundamentals and the RMT-first / loopback-test strategy come from the project's [LED driver analysis](../../../backlog/leddriver-analysis-top-down.md), which studies FastLED's `clockless_rmt_esp32`, hpwit's I2S drivers, and WLED — read for the lessons, not copied. RMT-v1-style manual ping-pong is what makes RMT more WiFi-resilient than the DMA-less v2 default ([FastLED #2082](https://github.com/FastLED/FastLED/issues/2082)).

## Source

[RmtLedDriver.h](../../../../src/light/drivers/RmtLedDriver.h)
