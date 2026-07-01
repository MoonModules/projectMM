# LCD LED Driver

Overview and controls: [drivers.md § LCD LED](drivers.md#lcdled). This page carries the reference detail a control list can't — 3-slot-per-bit wire contract, buffer slicing, DMA memory sizing, and cross-domain wiring.

## Wire contract — 3 slots per bit

Each WS2812 bit becomes three bus slots at 2.67 MHz (slot = 375 ns): all-active-lanes HIGH, then the data bits, then all LOW — so a `1` is HIGH 750 ns and a `0` 375 ns, approximating the [RMT driver's](RmtLedDriver.md) 700/350 ns timings. The slot is deliberately not the lineage's ~416 ns: newer WS2812B revisions spec T0H max ≈ 380 ns, and a longer `0` pulse on a direct 3.3 V data line gets misread as `1` (the strip washes out white). One 8-bit bus word per slot; bus bit L is the L-th pin of `pins`. Strands of unequal length idle LOW once exhausted (they are dropped from both the pulse-start and data slots — no white flashes on short strands). The ≥300 µs latch is **part of the DMA buffer** — driven zero bytes appended to the frame — so the lines rest actively LOW between frames. The full slot layout lives in [LcdSlots.h](../../../../src/light/drivers/LcdSlots.h).

Because the whole frame is pre-encoded into one DMA buffer off the hot path and the transfer runs autonomously, **no CPU deadline exists during transmission** — the WiFi-induced bit-slip that plagues refill-based drivers cannot occur by construction.

## Buffer slicing across pins

Identical semantics to the [RMT driver](RmtLedDriver.md#buffer-slicing-across-pins): consecutive slices of the source buffer in `pins` order, sizes from `ledsPerPin`, even-split remainder. The parsers are shared (`PinList.h`).

## Memory

One internal-RAM DMA frame buffer owned by the platform (PSRAM is deliberately not used — the peripheral streams from internal SRAM): `longest lane × channels × 24 + latch pad` bytes, ~72 B per RGB light. A 1000-light installation across 8 lanes ≈ 9 KB; the documented boundary is ~1500+ lights on a *single* lane (~110 KB), where a future streaming/PSRAM increment takes over. Allocation respects the platform heap reserve and degrades to a status error — never a crash.

## Cross-domain wiring

The driver is added as a child of the `Drivers` container at runtime via the catalog (`POST /api/modules`, a board's [`deviceModels.json`](../../../install/deviceModels.json) `modules` entry) — not boot-wired, so it only exists on a board that selects it. The type is registered on every target, but the peripheral exists only where the SOC has the LCD_CAM i80 bus (the S3 among current targets): on a chip without it the driver is inert (`lanesAvailable()` is 0, so init / loopback report "not supported on this platform"), so a board entry only lists `LcdLedDriver` where it makes sense. Once added, `Drivers::passBufferToDrivers` wires it like any child. The **slot encode** (`LcdSlots.h`) is domain code, host-testable; the **peripheral** (`platform_esp32_lcd.cpp`, ESP-IDF's [`esp_lcd` i80 bus](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/index.html) + GDMA) is the only IDF-touching part.

## Tests

Full case list in the generated [unit tests § LcdLedDriver](../../../tests/unit-tests.md#lcdleddriver) (regenerated from the test files, never drifts). What's covered:

- **Encoder (CI, host):** byte-exact 3-slot triplets — transpose across lanes, MSB-first, the unequal-lane idle-LOW rule, GRB via Correction, RGBW rows.
- **Driver (CI, host):** lane slicing (including unequal leds-per-lane), frame-byte math (RGBW growth, alignment rounding), bad-pin status + recovery, the exactly-8-pins rule, the empty-default idle (no GPIO claimed until pins are set), zero-grid robustness, teardown.
- **`loopbackTxPin` control (CI, host):** the conditional control — bound always, shown only while `loopbackTest` is on. The lane-0 override mechanism is shared with the Parlio driver (same `ParallelLedDriver` base) and hardware-verified there; the LCD hardware path itself is exercised by the loopback self-test above. The catalog-add path is verified on the sibling RMT/Parlio drivers (S3 boards currently default to RMT — LcdLed needs all 8 lanes).
- **Hardware:** the loopback self-test above (jumper), and tick-scaling across grid sizes proves frames really clock out.

## Prior art

The LCD_CAM-for-WS2812 repurposing was discovered by **Adafruit (Phil Burgess)** ([ESP32uesday, June 2022](https://blog.adafruit.com/2022/06/14/esp32uesday-hacking-the-esp32-s3-lcd-peripheral/)) and matured in **hpwit's I2SClockless driver lineage** ([I2SClocklessVirtualLedDriver](https://github.com/hpwit/I2SClocklessVirtualLedDriver)) and **FastLED's S3 clockless-LCD driver** — studied via the project's [LED driver analyses](../../../backlog/leddriver-analysis-top-down.md) for the lessons, never copied. This driver differs from that lineage by pre-encoding the whole frame (no ISR-refilled ring), trading a larger buffer for the absence of refill deadlines.

## Source

[LcdLedDriver.h](../../../../src/light/drivers/LcdLedDriver.h)
