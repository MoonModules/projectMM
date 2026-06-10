# LCD LED Driver

Parallel WS2812B output on the **ESP32-S3** over the LCD_CAM peripheral: up to **8 strands clock out simultaneously**, one GPIO per strand, all fed by a single autonomous DMA transfer. The S3's scale path — where the [RMT driver](RmtLedDriver.md) tops out at 4 channels on the S3, this drives 8 lanes for the wall time of one. Reads the Drivers container's buffer, applies the shared [Correction](Correction.md) per light, and bit-transposes corrected bytes across the lanes.

## Wire contract — 3 slots per bit

Each WS2812 bit becomes three bus slots at 2.67 MHz (slot = 375 ns): all-active-lanes HIGH, then the data bits, then all LOW — so a `1` is HIGH 750 ns and a `0` 375 ns, approximating the [RMT driver's](RmtLedDriver.md) 700/350 ns timings. The slot is deliberately not the lineage's ~416 ns: newer WS2812B revisions spec T0H max ≈ 380 ns, and a longer `0` pulse on a direct 3.3 V data line gets misread as `1` (the strip washes out white). One 8-bit bus word per slot; bus bit L is the L-th pin of `pins`. Strands of unequal length idle LOW once exhausted (they are dropped from both the pulse-start and data slots — no white flashes on short strands). The ≥300 µs latch is **part of the DMA buffer** — driven zero bytes appended to the frame — so the lines rest actively LOW between frames. The full slot layout lives in [LcdSlots.h](../../../../src/light/drivers/LcdSlots.h).

Because the whole frame is pre-encoded into one DMA buffer off the hot path and the transfer runs autonomously, **no CPU deadline exists during transmission** — the WiFi-induced bit-slip that plagues refill-based drivers cannot occur by construction.

## Buffer slicing across pins

Identical semantics to the [RMT driver](RmtLedDriver.md#buffer-slicing-across-pins): consecutive slices of the source buffer in `pins` order, sizes from `ledsPerPin`, even-split remainder. The parsers are shared (`PinList.h`).

## Controls

- `pins` (text, default `"1,2,4,5,6,7,8,9"`) — comma-separated data GPIOs, one lane each, **exactly 8** (the i80 peripheral configures every data line of the bus width and rejects partial sets, so all 8 GPIOs are claimed even when fewer strands are wired). Defaults avoid the LOLIN S3's octal-PSRAM pins (26–37), USB (19/20) and strapping pins. Changing it re-creates the bus live.
- `ledsPerPin` (text, default empty) — lights per lane, matched by position; empty = even split. To drive fewer than 8 strands, give the unused lanes `0` (or list only the used lanes' counts summing to the grid size — the remainder lanes get 0 and idle LOW).
- `clockPin` (uint16_t, default 10) — the i80 bus WR line. The peripheral requires it on a real GPIO; WS2812 strands ignore it (sacrificial pin).
- `dcPin` (uint16_t, default 11) — the i80 data/command line, same story: required by the peripheral, unused by the LEDs.
- `loopbackTest` (bool) — one-shot signal self-test: jumper the **first** pin in `pins` to `loopbackRxPin`, tick the box; the driver transmits its **real frame** (full size, real DMA chain, repeated back to back like the render loop) with a known pattern in every row, captures the whole frame back with an RMT RX channel (the increment-1 rig reused — RMT receive is transmitter-agnostic) and verifies every bit. Result lands in the status field; on failure it names the first corrupted light.
- `loopbackRxPin` (uint16_t, default 12) — the RX pin for the self-test. Shown only while `loopbackTest` is on.

## Memory

One internal-RAM DMA frame buffer owned by the platform (PSRAM is deliberately not used — the peripheral streams from internal SRAM): `longest lane × channels × 24 + latch pad` bytes, ~72 B per RGB light. A 1000-light installation across 8 lanes ≈ 9 KB; the documented boundary is ~1500+ lights on a *single* lane (~110 KB), where a future streaming/PSRAM increment takes over. Allocation respects the platform heap reserve and degrades to a status error — never a crash.

## Cross-domain wiring

Added as a child of the `Drivers` container in `main.cpp` under `if constexpr (platform::lcdLanes > 0)` (SOC-derived: the S3 among current targets), wired by code like its siblings. The **slot encode** (`LcdSlots.h`) is domain code, host-testable; the **peripheral** (`platform_esp32_lcd.cpp`, ESP-IDF's `esp_lcd` i80 bus + GDMA) is the only IDF-touching part.

## Tests

- **Encoder (CI, host):** `test/unit/light/unit_LcdLedEncoder.cpp` — byte-exact 3-slot triplets: transpose across lanes, MSB-first, the unequal-lane idle-LOW rule, GRB via Correction, RGBW rows.
- **Driver (CI, host):** `test/unit/light/unit_LcdLedDriver.cpp` — lane slicing, frame-byte math (incl. RGBW growth and alignment rounding), bad-pin status + recovery, the exactly-8-pins rule, zero-grid robustness, teardown.
- **Hardware:** the loopback self-test above (jumper), and tick-scaling across grid sizes proves frames really clock out.

## Prior art

The LCD_CAM-for-WS2812 repurposing was discovered by **Adafruit (Phil Burgess)** ([ESP32uesday, June 2022](https://blog.adafruit.com/2022/06/14/esp32uesday-hacking-the-esp32-s3-lcd-peripheral/)) and matured in **hpwit's I2SClockless driver lineage** ([I2SClocklessVirtualLedDriver](https://github.com/hpwit/I2SClocklessVirtualLedDriver)) and **FastLED's S3 clockless-LCD driver** — studied via the project's [LED driver analyses](../../../backlog/leddriver-analysis-top-down.md) for the lessons, never copied. This driver differs from that lineage by pre-encoding the whole frame (no ISR-refilled ring), trading a larger buffer for the absence of refill deadlines.

## Source

[LcdLedDriver.h](../../../../src/light/drivers/LcdLedDriver.h)
