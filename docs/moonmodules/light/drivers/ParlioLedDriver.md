# Parlio LED Driver

Parallel WS2812B output on the **ESP32-P4** over the Parlio (Parallel IO) TX peripheral: up to **8 strands clock out simultaneously**, one GPIO per strand, all fed by a single autonomous DMA transfer. The P4's scale path — the sibling of the [LCD driver](LcdLedDriver.md) on the S3. Reads the Drivers container's buffer, applies the shared [Correction](Correction.md) per light, and bit-transposes corrected bytes across the lanes.

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

- **No clock/dc pins.** The i80 bus needs two sacrificial GPIOs (WR + DC) on real pins even though WS2812 ignores them; Parlio generates the pixel clock itself, so there are none.
- **No exactly-8-pins rule.** The i80 layer rejects a partial bus (every data line must be a real GPIO), so the LCD driver demands exactly 8 pins. Parlio takes the data GPIOs directly and runs on **1–8 lanes** — whatever `pins` names.

## Wire contract — 3 slots per bit

Identical to the [LCD driver's](LcdLedDriver.md#wire-contract--3-slots-per-bit): each WS2812 bit becomes three bus slots at 2.67 MHz (slot = 375 ns) — all-active-lanes HIGH, the data bits, then all LOW — so a `1` is HIGH 750 ns and a `0` 375 ns. The 375 ns slot (not the lineage's ~416 ns) keeps T0H inside newer WS2812B revisions' ~380 ns window; the P4 Parlio's 160 MHz PLL clock divides to it exactly (÷60). One 8-bit bus word per slot, bus bit L = the L-th pin of `pins`; short strands idle LOW once exhausted. The ≥300 µs latch is zeroed trailing bytes of the DMA buffer, so the lines rest actively LOW between frames. The encoder is **shared with the LCD driver** — a Parlio bus byte and an i80 bus byte are identical — and lives in [LcdSlots.h](../../../../src/light/drivers/LcdSlots.h).

Because the whole frame is pre-encoded into one DMA buffer off the hot path and the transfer runs autonomously (single-shot, not Parlio's loop-transmission mode), **no CPU deadline exists during transmission** — the WiFi-induced bit-slip of refill-based drivers cannot occur by construction.

## Buffer slicing across pins

Identical semantics to the [RMT driver](RmtLedDriver.md#buffer-slicing-across-pins): consecutive slices of the source buffer in `pins` order, sizes from `ledsPerPin`, even-split remainder. The parsers are shared (`PinList.h`).

## Controls

- `pins` (text, default `"20"`) — comma-separated data GPIOs, one lane each, **1 to 8** (no all-pins rule). Default is a **single lane** — a typical 8×8 panel is one serpentine 64-LED strand. Choosing pins on the P4-NANO, **avoid**: STRAPPING pins **34–38** (boot-mode control — driving these can break boot, never use them for output), Ethernet RMII (28–31, 49–52), the ESP32-C6 SDIO (14–19, 54), and I2C (7–8). The clear GPIOs are **20–27, 32–33, 39–48**; the default 20 is strapping-safe. Add more pins for parallel strips. Changing it re-creates the TX unit live. The loopback self-test transmits on the **first** pin.
- `ledsPerPin` (text, default `"64"`) — lights per lane, matched by position; empty = even split, remainder to the last lane. 64 = a one-strand 8×8 panel.
- `loopbackTest` (bool) — one-shot signal self-test using a **dedicated** TX/RX pin pair (not the strip's data line): TX on the first pin in `pins`, RX on `loopbackRxPin`. **Wired but not yet implemented** — the body lands in round 4 (Parlio RX or RMT-RX capture, like the [RMT](RmtLedDriver.md#loopback-self-test-on-device) / LCD loopbacks); for now ticking it reports "not implemented yet (round 4)".
- `loopbackRxPin` (uint16_t, default 33) — the RX pin for the self-test (jumper GPIO 32 → 33). Both strapping-safe. Shown only while `loopbackTest` is on.

## Memory

One internal-RAM DMA frame buffer owned by the platform (PSRAM is deliberately not used — Parlio streams from internal SRAM): `longest lane × channels × 24 + latch pad` bytes, ~72 B per RGB light, same sizing as the LCD driver. A 1000-light installation across 8 lanes ≈ 9 KB; the ~1500+-lights-on-a-single-lane (~110 KB) boundary where a future streaming/PSRAM increment takes over is the same documented limit. Allocation respects the platform heap reserve and degrades to a status error — never a crash.

## Cross-domain wiring

Added as a child of the `Drivers` container in `main.cpp` under `if constexpr (platform::parlioLanes > 0)` (SOC-derived: the P4 among current targets), wired by code like its siblings. The **slot encode** ([LcdSlots.h](../../../../src/light/drivers/LcdSlots.h), shared) is domain code, host-testable; the **peripheral** (`platform_esp32_parlio.cpp`, ESP-IDF's `esp_driver_parlio` TX unit + DMA) is the only IDF-touching part.

## Tests

- **Encoder (CI, host):** shared with the LCD driver — `test/unit/light/unit_LcdLedEncoder.cpp` covers the 3-slot byte layout; not re-tested here.
- **Driver (CI, host):** `test/unit/light/unit_ParlioLedDriver.cpp` — lane slicing, frame-byte math (RGBW growth, alignment rounding, latch pad), the **1–8 lanes accepted** rule (the Parlio-vs-i80 difference), over-8 rejection, bad-pin status + recovery, zero-grid + loop() crash-safety, teardown.
- **Hardware:** tick-scaling across grid sizes proves frames clock out (round 2); the loopback self-test + a real strip land in round 4.

## Prior art

The P4 **Parlio** peripheral is Espressif's dedicated parallel-output engine (`esp_driver_parlio`). The parallel-WS2812 technique is the same studied for the LCD driver — **hpwit's I2SClockless lineage** and **FastLED's parallel clockless drivers** — read for the lessons via the project's [LED driver analyses](../../../backlog/leddriver-analysis-top-down.md), never copied. Like the LCD driver, this one pre-encodes the whole frame (no ISR-refilled ring), trading a larger buffer for the absence of refill deadlines.

## Source

[ParlioLedDriver.h](../../../../src/light/drivers/ParlioLedDriver.h)
