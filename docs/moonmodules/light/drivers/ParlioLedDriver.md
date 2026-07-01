# Parlio LED Driver

Overview and controls: [drivers.md § Parlio LED](drivers.md#parlioled). This page carries the reference detail a control list can't — the P4 pin budget (all three LED peripherals at once), the shared 3-slot wire contract, memory sizing, and cross-domain wiring.

## Wire contract — 3 slots per bit

Identical to the [LCD driver's](LcdLedDriver.md#wire-contract--3-slots-per-bit): each WS2812 bit becomes three bus slots at 2.67 MHz (slot = 375 ns) — all-active-lanes HIGH, the data bits, then all LOW — so a `1` is HIGH 750 ns and a `0` 375 ns. The 375 ns slot (not the lineage's ~416 ns) keeps T0H inside newer WS2812B revisions' ~380 ns window; the P4 Parlio's 160 MHz PLL clock divides to it exactly (÷60). One 8-bit bus word per slot, bus bit L = the L-th pin of `pins`; short strands idle LOW once exhausted. The ≥300 µs latch is zeroed trailing bytes of the DMA buffer, so the lines rest actively LOW between frames. The encoder is **shared with the LCD driver** — a Parlio bus byte and an i80 bus byte are identical — and lives in [LcdSlots.h](../../../../src/light/drivers/LcdSlots.h).

Because the whole frame is pre-encoded into one DMA buffer off the hot path and the transfer runs autonomously (single-shot, not Parlio's loop-transmission mode), **no CPU deadline exists during transmission** — the WiFi-induced bit-slip of refill-based drivers cannot occur by construction.

## Buffer slicing across pins

Identical semantics to the [RMT driver](RmtLedDriver.md#buffer-slicing-across-pins): consecutive slices of the source buffer in `pins` order, sizes from `ledsPerPin`, even-split remainder. The parsers are shared (`PinList.h`).

## Memory

One internal-RAM DMA frame buffer owned by the platform (PSRAM is deliberately not used — Parlio streams from internal SRAM): `longest lane × channels × 24 + latch pad` bytes, ~72 B per RGB light, same sizing as the LCD driver. A 1000-light installation across 8 lanes ≈ 9 KB; the ~1500+-lights-on-a-single-lane (~110 KB) boundary where a future streaming/PSRAM increment takes over is the same documented limit. Allocation respects the platform heap reserve and degrades to a status error — never a crash.

## Cross-domain wiring

The driver is added as a child of the `Drivers` container at runtime via the catalog (`POST /api/modules`, a board's [`deviceModels.json`](../../../install/deviceModels.json) `modules` entry) — not boot-wired, so it only exists on a board that selects it. The type is registered on every target, but the peripheral exists only where the SOC has the Parlio TX unit (the P4 among current targets): on a chip without it the driver is inert (`lanesAvailable()` is 0), so a board entry only lists `ParlioLedDriver` where it makes sense. Once added, `Drivers::passBufferToDrivers` wires it like any child. The **slot encode** ([LcdSlots.h](../../../../src/light/drivers/LcdSlots.h), shared) is domain code, host-testable; the **peripheral** (`platform_esp32_parlio.cpp`, ESP-IDF's `esp_driver_parlio` TX unit + DMA) is the only IDF-touching part.

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
