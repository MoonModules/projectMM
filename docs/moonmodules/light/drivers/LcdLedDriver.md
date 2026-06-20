# LCD LED Driver

Parallel [WS2812B](https://cdn-shop.adafruit.com/datasheets/WS2812B.pdf) output on the **ESP32-S3** over the [LCD_CAM](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/index.html) peripheral: up to **8 strands clock out simultaneously**, one GPIO per strand, all fed by a single autonomous DMA transfer. The S3's scale path — where the [RMT driver](RmtLedDriver.md) tops out at 4 channels on the S3, this drives 8 lanes for the wall time of one. Reads the Drivers container's buffer, applies the shared [Correction](Correction.md) per light, and bit-transposes corrected bytes across the lanes.

## Wire contract — 3 slots per bit

Each WS2812 bit becomes three bus slots at 2.67 MHz (slot = 375 ns): all-active-lanes HIGH, then the data bits, then all LOW — so a `1` is HIGH 750 ns and a `0` 375 ns, approximating the [RMT driver's](RmtLedDriver.md) 700/350 ns timings. The slot is deliberately not the lineage's ~416 ns: newer WS2812B revisions spec T0H max ≈ 380 ns, and a longer `0` pulse on a direct 3.3 V data line gets misread as `1` (the strip washes out white). One 8-bit bus word per slot; bus bit L is the L-th pin of `pins`. Strands of unequal length idle LOW once exhausted (they are dropped from both the pulse-start and data slots — no white flashes on short strands). The ≥300 µs latch is **part of the DMA buffer** — driven zero bytes appended to the frame — so the lines rest actively LOW between frames. The full slot layout lives in [LcdSlots.h](../../../../src/light/drivers/LcdSlots.h).

Because the whole frame is pre-encoded into one DMA buffer off the hot path and the transfer runs autonomously, **no CPU deadline exists during transmission** — the WiFi-induced bit-slip that plagues refill-based drivers cannot occur by construction.

## Buffer slicing across pins

Identical semantics to the [RMT driver](RmtLedDriver.md#buffer-slicing-across-pins): consecutive slices of the source buffer in `pins` order, sizes from `ledsPerPin`, even-split remainder. The parsers are shared (`PinList.h`).

## Controls

- `pins` (text, default empty) — comma-separated data GPIOs, one lane each, **exactly 8** (the i80 peripheral configures every data line of the bus width and rejects partial sets, so all 8 GPIOs are claimed even when fewer strands are wired). Empty by default (the strand is user-soldered, so no pin is assumed — the driver idles until set); a known-good ESP32-S3 N16R8 Dev set is `1,2,4,5,6,7,8,9`, which clears the octal-PSRAM pins (26–37), USB (19/20) and strapping pins. Changing it re-creates the i80 bus **live, no reboot** ([§ Live reconfiguration](../../../architecture.md#live-reconfiguration-every-change-applies-without-a-reboot)).
- `ledsPerPin` (text, default empty) — lights per lane, matched by position; empty = even split. To drive fewer than 8 strands, give the unused lanes `0` (or list only the used lanes' counts summing to the grid size — the remainder lanes get 0 and idle LOW).
- `clockPin` (pin, default 10) — the i80 bus WR line. The peripheral *requires* it on a real GPIO (the IDF i80 bus rejects `wr_gpio_num < 0`); WS2812 strands ignore the waveform. Peripheral-fixed (not user-strand wiring), so it keeps a sensible overridable default — point it at any otherwise-free GPIO if 10 is taken.
- `dcPin` (pin, default 11) — the i80 data/command line, same story: required by the peripheral (`dc_gpio_num < 0` is rejected), unused by the LEDs, overridable default.
- `loopbackTest` (bool) — one-shot signal self-test: jumper the **first** pin in `pins` to `loopbackRxPin`, tick the box; the driver transmits its **real frame** (full size, real DMA chain, repeated back to back like the render loop) with a known pattern in every row, captures the whole frame back with an RMT RX channel (the increment-1 rig reused — RMT receive is transmitter-agnostic) and verifies every bit. Result lands in the status field; on failure it names the first corrupted light.
- `loopbackTxPin` (pin, default unset / −1) — optional **TX override** for the self-test: the loopback drives only lane 0 with the test pattern, so when this is set it transmits on this pin in place of lane 0 (`pins[0]`), the other 7 lanes unchanged — letting the test run on a dedicated jumper without re-typing `pins`. Falls back to lane 0 when unset. Test-only — normal output uses `pins`. Shown only while `loopbackTest` is on.
- `loopbackRxPin` (pin, default unset / −1) — the RX pin for the self-test; set it when you wire the jumper (the bench used 12). Shown only while `loopbackTest` is on.

## Memory

One internal-RAM DMA frame buffer owned by the platform (PSRAM is deliberately not used — the peripheral streams from internal SRAM): `longest lane × channels × 24 + latch pad` bytes, ~72 B per RGB light. A 1000-light installation across 8 lanes ≈ 9 KB; the documented boundary is ~1500+ lights on a *single* lane (~110 KB), where a future streaming/PSRAM increment takes over. Allocation respects the platform heap reserve and degrades to a status error — never a crash.

## Cross-domain wiring

The driver is added as a child of the `Drivers` container at runtime via the catalog (`POST /api/modules`, a board's [`deviceModels.json`](../../../install/deviceModels.json) `modules` entry) — not boot-wired, so it only exists on a board that selects it. The type is registered on every target, but the peripheral exists only where the SOC has the LCD_CAM i80 bus (the S3 among current targets): on a chip without it the driver is inert (`lanesAvailable()` is 0, so init / loopback report "not supported on this platform"), so a board entry only lists `LcdLedDriver` where it makes sense. Once added, `Drivers::passBufferToDrivers` wires it like any child. The **slot encode** (`LcdSlots.h`) is domain code, host-testable; the **peripheral** (`platform_esp32_lcd.cpp`, ESP-IDF's [`esp_lcd` i80 bus](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/index.html) + GDMA) is the only IDF-touching part.

## Tests

Full case list in the generated [unit tests § LcdLedDriver](../../../tests/unit-tests.md#lcdleddriver) (regenerated from the test files, never drifts). What's covered:

- **Encoder (CI, host):** byte-exact 3-slot triplets — transpose across lanes, MSB-first, the unequal-lane idle-LOW rule, GRB via Correction, RGBW rows.
- **Driver (CI, host):** lane slicing (including unequal leds-per-lane), frame-byte math (RGBW growth, alignment rounding), bad-pin status + recovery, the exactly-8-pins rule, the empty-default idle (no GPIO claimed until pins are set), zero-grid robustness, teardown.
- **`loopbackTxPin` control (CI, host):** the conditional control — bound always, shown only while `loopbackTest` is on. The lane-0 override mechanism is shared with the Parlio driver (same `ParallelLedDriver` base) and hardware-verified there; the LCD hardware path itself is exercised by the loopback self-test above. The catalog-add path is verified on the sibling RMT/Parlio drivers (S3 boards currently default to RMT — LcdLed needs all 8 lanes, see the [backlog 1..8-pin LCD note](../../../backlog/backlog.md)).
- **Hardware:** the loopback self-test above (jumper), and tick-scaling across grid sizes proves frames really clock out.

## Prior art

The LCD_CAM-for-WS2812 repurposing was discovered by **Adafruit (Phil Burgess)** ([ESP32uesday, June 2022](https://blog.adafruit.com/2022/06/14/esp32uesday-hacking-the-esp32-s3-lcd-peripheral/)) and matured in **hpwit's I2SClockless driver lineage** ([I2SClocklessVirtualLedDriver](https://github.com/hpwit/I2SClocklessVirtualLedDriver)) and **FastLED's S3 clockless-LCD driver** — studied via the project's [LED driver analyses](../../../backlog/leddriver-analysis-top-down.md) for the lessons, never copied. This driver differs from that lineage by pre-encoding the whole frame (no ISR-refilled ring), trading a larger buffer for the absence of refill deadlines.

## Source

[LcdLedDriver.h](../../../../src/light/drivers/LcdLedDriver.h)
