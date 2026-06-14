# Parlio LED Driver

Parallel [WS2812B](https://cdn-shop.adafruit.com/datasheets/WS2812B.pdf) output on the **[ESP32-P4](https://www.espressif.com/en/products/socs/esp32-p4)** over the [Parlio (Parallel IO)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/parlio/index.html) TX peripheral: up to **8 strands clock out simultaneously**, one GPIO per strand, all fed by a single autonomous DMA transfer. The P4's scale path — the sibling of the [LCD driver](LcdLedDriver.md) on the S3. Reads the Drivers container's buffer, applies the shared [Correction](Correction.md) per light, and bit-transposes corrected bytes across the lanes.

The P4 actually carries **all three** LED peripherals — [RMT](RmtLedDriver.md) (4 DMA-backed channels), [LCD_CAM i80](LcdLedDriver.md) (8 lanes), and Parlio (this driver) — and all three drivers auto-wire there off their SOC-capability gates. Parlio is the **preferred** parallel path on the P4 (its dedicated parallel-output engine), with RMT the easy single-strand option and LCD_CAM the other parallel route; the user picks per install by enabling the driver that matches their wiring.

### Running all three at once — the P4 pin budget

The three drivers are independent children of the `Drivers` container: each has its own `pins`, each reads the same logical buffer, and they're separate peripherals (RMT, LCD_CAM, and Parlio are distinct engines), so **they can all transmit simultaneously, each on its own GPIOs**. The combined output-pin ceiling, with the drivers' current per-chip caps:

| Driver | Pins (current cap) | Peripheral max |
|---|---|---|
| RMT | 4 (the P4 has 4 TX channels) | 4 |
| LCD_CAM i80 | 8 | 16 |
| Parlio | 8 (any 1–8) | 16 |
| **Total simultaneous** | **20** | up to 36 if the LCD/Parlio caps were raised |

So **up to 20 parallel WS2812 strands** at once on the P4 today. The Waveshare P4-NANO physically exposes exactly 20 clear GPIOs (`20–27, 32–33, 39–48`, after Ethernet, the C6 SDIO, I2C and the strapping pins), so that board can in principle drive all 20 — but two honest limits apply beyond pin count: (1) **throughput is bounded by internal DMA RAM and the render tick, not pins** — the per-frame DMA buffers (~72 B/RGB light per parallel driver) and the encode time set the real ceiling on *long* strands, so 20 short strands is very different from 20 long ones; and (2) raising the LCD/Parlio caps to 16 (a constant change) only helps where a board breaks out that many free pins, which the P4-NANO does not. For most installs one parallel driver (8 lanes) is plenty; the multi-driver headroom is there for unusually wide, short-strand layouts.

It is the [LCD driver](LcdLedDriver.md) shape with two simplifications, because Parlio is a simpler peripheral than the LCD_CAM i80 bus:

- **No clock/dc pins.** The i80 bus needs two GPIOs (WR + DC) on real pins even though WS2812 ignores them (the IDF i80 layer rejects `wr/dc < 0`); Parlio generates the pixel clock itself (`clk_out_gpio_num = GPIO_NUM_NC`) and has no command/data phase, so there are none. (The LCD driver keeps an overridable default for its two; dropping them there would need a direct-LCD_CAM driver — backlogged.)
- **No exactly-8-pins rule.** The i80 layer rejects a partial bus (every data line must be a real GPIO), so the LCD driver demands exactly 8 pins. Parlio takes the data GPIOs directly and runs on **1–8 lanes** — whatever `pins` names.

## Wire contract — 3 slots per bit

Identical to the [LCD driver's](LcdLedDriver.md#wire-contract--3-slots-per-bit): each WS2812 bit becomes three bus slots at 2.67 MHz (slot = 375 ns) — all-active-lanes HIGH, the data bits, then all LOW — so a `1` is HIGH 750 ns and a `0` 375 ns. The 375 ns slot (not the lineage's ~416 ns) keeps T0H inside newer WS2812B revisions' ~380 ns window; the P4 Parlio's 160 MHz PLL clock divides to it exactly (÷60). One 8-bit bus word per slot, bus bit L = the L-th pin of `pins`; short strands idle LOW once exhausted. The ≥300 µs latch is zeroed trailing bytes of the DMA buffer, so the lines rest actively LOW between frames. The encoder is **shared with the LCD driver** — a Parlio bus byte and an i80 bus byte are identical — and lives in [LcdSlots.h](../../../../src/light/drivers/LcdSlots.h).

Because the whole frame is pre-encoded into one DMA buffer off the hot path and the transfer runs autonomously (single-shot, not Parlio's loop-transmission mode), **no CPU deadline exists during transmission** — the WiFi-induced bit-slip of refill-based drivers cannot occur by construction.

## Buffer slicing across pins

Identical semantics to the [RMT driver](RmtLedDriver.md#buffer-slicing-across-pins): consecutive slices of the source buffer in `pins` order, sizes from `ledsPerPin`, even-split remainder. The parsers are shared (`PinList.h`).

## Controls

- `pins` (text, default empty) — comma-separated data GPIOs, one lane each, **1 to 8** (no all-pins rule). Empty by default (the strand is user-soldered, so no pin is assumed — the driver idles until set). Choosing pins on the P4-NANO, **avoid**: STRAPPING pins **34–38** (boot-mode control — driving these can break boot, never use them for output), Ethernet RMII (28–31, 49–52), the ESP32-C6 SDIO (14–19, 54), and I2C (7–8). The clear GPIOs are **20–27, 32–33, 39–48**; a known-good bench set is `20,21,22,23,24,25,26,27`. Add pins for parallel strips. Changing it re-creates the Parlio TX unit **live, no reboot** ([§ Live reconfiguration](../../../architecture.md#live-reconfiguration-every-change-applies-without-a-reboot)). The loopback self-test transmits on the **first** pin.
- `ledsPerPin` (text, default empty) — lights per lane, matched by position; empty = even split over the wired lanes (all lights on the first lane when one pin is set), remainder to the last lane. Same semantics as the RMT/LCD drivers.
- `loopbackTest` (bool) — one-shot **whole-frame** signal self-test: TX on the first pin in `pins`, RX on `loopbackRxPin`. It builds the real frame (test pattern in every row on lane 0), transmits it back to back like the render loop through a private Parlio TX unit, captures the entire frame on the RX pin (RMT-RX with the P4's DMA backend — the [same `rmtWs2812RxCapture`](RmtLedDriver.md#loopback-self-test-on-device) the RMT/LCD rigs use, transmitter-agnostic), and bit-verifies every WS2812 bit. The verdict lands in the status field: `loopback PASS`, `loopback FAIL: bad bit N/M (light K)`, or `loopback: jumper not detected` (a plain-GPIO continuity pre-check runs first). The strip on lane 0 flickers once during the run; normal output resumes after.
- `loopbackTxPin` (uint16_t, default unset) — optional **TX override** for the self-test: the loopback drives only lane 0 with the test pattern, so when this is set it transmits on this pin in place of lane 0 (`pins[0]`), the other lanes unchanged — letting the test run on a dedicated jumper without re-typing `pins`. Falls back to lane 0 when unset. Test-only — normal output uses `pins`. Shown only while `loopbackTest` is on.
- `loopbackRxPin` (uint16_t, default unset) — the RX pin for the self-test; set it when you wire the jumper (the bench used 33, jumper the TX pin → 32, both strapping-safe). Shown only while `loopbackTest` is on.

## Memory

One internal-RAM DMA frame buffer owned by the platform (PSRAM is deliberately not used — Parlio streams from internal SRAM): `longest lane × channels × 24 + latch pad` bytes, ~72 B per RGB light, same sizing as the LCD driver. A 1000-light installation across 8 lanes ≈ 9 KB; the ~1500+-lights-on-a-single-lane (~110 KB) boundary where a future streaming/PSRAM increment takes over is the same documented limit. Allocation respects the platform heap reserve and degrades to a status error — never a crash.

## Cross-domain wiring

The driver is added as a child of the `Drivers` container at runtime via the catalog (`POST /api/modules`, a board's [`boards.json`](../../../install/boards.json) `modules` entry) — not boot-wired, so it only exists on a board that selects it. The type is registered on every target, but the peripheral exists only where the SOC has the Parlio TX unit (the P4 among current targets): on a chip without it the driver is inert (`lanesAvailable()` is 0), so a board entry only lists `ParlioLedDriver` where it makes sense. Once added, `Drivers::passBufferToDrivers` wires it like any child. The **slot encode** ([LcdSlots.h](../../../../src/light/drivers/LcdSlots.h), shared) is domain code, host-testable; the **peripheral** (`platform_esp32_parlio.cpp`, ESP-IDF's `esp_driver_parlio` TX unit + DMA) is the only IDF-touching part.

## Tests

Full case list in the generated [unit tests § ParlioLedDriver](../../../tests/unit-tests.md#parlioleddriver) (regenerated from the test files, never drifts). What's covered:

- **Encoder (CI, host):** shared with the LCD driver — the 3-slot byte layout is covered under [LcdLedDriver](LcdLedDriver.md#tests); not re-tested here.
- **Driver (CI, host):** lane slicing (including unequal leds-per-lane), frame-byte math (RGBW growth, alignment rounding, latch pad), the **1–8 lanes accepted** rule (the Parlio-vs-i80 difference), over-8 rejection, bad-pin status + recovery, the empty-default idle (no GPIO claimed until pins are set), zero-grid + loop() crash-safety, teardown.
- **`loopbackTxPin` control (CI, host):** the conditional control — bound always, shown only while `loopbackTest` is on.
- **Hardware:** tick-scaling across grid sizes proves frames clock out; the whole-frame loopback self-test bit-verifies the wire signal on the P4. The driver is catalog-added (not boot-wired) — verified on the P4-NANO bench: a fresh-erased board has no `ParlioLed` until a board is selected, a catalog inject creates it under `Drivers` (`pins`=20–27, `ledsPerPin`=64, `loopbackTxPin`=33, `loopbackRxPin`=32), and it persists across a reboot. The loopback drives only lane 0, so `loopbackTxPin` substitutes for lane 0 (TX jumper 33 → RX 32) without retyping `pins`.

## Prior art

The P4 **Parlio** peripheral is Espressif's dedicated parallel-output engine (`esp_driver_parlio`). The parallel-WS2812 technique is the same studied for the LCD driver — **hpwit's I2SClockless lineage** and **FastLED's parallel clockless drivers** — read for the lessons via the project's [LED driver analyses](../../../backlog/leddriver-analysis-top-down.md), never copied. Like the LCD driver, this one pre-encodes the whole frame (no ISR-refilled ring), trading a larger buffer for the absence of refill deadlines.

## Source

[ParlioLedDriver.h](../../../../src/light/drivers/ParlioLedDriver.h)
