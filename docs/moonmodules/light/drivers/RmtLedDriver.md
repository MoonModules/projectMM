# RMT LED Driver

Output driver for WS2812B-class addressable LEDs over the ESP32 **[RMT (Remote Control Transceiver)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/rmt.html)** peripheral — one or more strands, one GPIO and one RMT TX channel per strand. Reads the Drivers container's buffer, applies the shared [Correction](Correction.md) (brightness / channel order / RGBW white) per light, and emits the WS2812 1-wire signal. Runs on any chip whose RMT peripheral has TX channels: classic ESP32 (8 channels), ESP32-S3 (4 channels), and ESP32-P4 (4 channels, DMA-backed). On desktop the RMT platform seam is a no-op and the driver is inert.

## Wire contract — [WS2812B](https://cdn-shop.adafruit.com/datasheets/WS2812B.pdf)

1-wire NRZ at 800 kHz, no clock line. Each data bit is a 1.25 µs cell that starts HIGH then drops LOW; the HIGH duration encodes the bit:

| | HIGH | period | meaning |
|---|---|---|---|
| `0` bit | 350 ns | 1250 ns | short high, long low |
| `1` bit | 700 ns | 1250 ns | long high, short low |

Bits are sent **MSB-first** within each byte; channel order (GRB, GRBW, …) is the light preset applied by `Correction` before the encode, so the encoder itself is order-agnostic. Frames are latched by ≥ 300 µs idle-LOW (current WS2812B/SK6812 silicon — the old 50 µs value is dead). These timings live in `LedDriverConfig` and are converted to RMT ticks from the peripheral's granted resolution (≈ 40 MHz / 25 ns per tick), so they are not hard-coded to one clock.

## Buffer slicing across pins

The source buffer is split into **consecutive slices**, one per pin, in list order: pin 1 takes lights `[0, n₁)`, pin 2 takes `[n₁, n₁+n₂)`, and so on. Slice sizes come from `ledsPerPin`; pins without an explicit count split the unassigned remainder evenly (the last pin takes the rounding remainder). Counts are clamped so the sum never exceeds the buffer; lights beyond the last slice are not emitted. With `ledsPerPin` empty the whole buffer splits evenly over all pins — the zero-config case.

## Concurrent show (blocks the render tick for the longest strand)

`loop()` encodes the whole frame once, then starts every pin's transmission (`platform::rmtWs2812Transmit`) before waiting on each (`platform::rmtWs2812Wait`) — the RMT channels clock out concurrently, so the render tick is charged roughly the **longest** strand, not the sum (~3 ms per 100 pixels on the longest slice), plus one shared reset gap. A dedicated core-1 driver task (the WiFi-glitch mitigation from the [LED driver analysis](../../../backlog/leddriver-analysis-top-down.md) § 7.2) and per-module core affinity are a later increment; until then, large strands or WiFi-interrupt-sensitive installs may show timing artifacts. See the [increment-2 plan](../../../backlog/leddriver-increment-2-plan.md).

## Controls

- `pins` (text, default empty) — comma-separated data / TX GPIO list, e.g. `18,17,16`. Empty by default (the strand is user-soldered, so no pin is assumed — the driver idles until set; the bench used `18`). One RMT TX channel per pin: up to 8 on classic ESP32, 4 on the S3 and P4 (exceeding the chip's limit, a bad token, or a duplicate pin puts an error in the status field and the driver idles). Changing it re-initialises the channels **live, no reboot** ([§ Live reconfiguration](../../../architecture.md#live-reconfiguration-every-change-applies-without-a-reboot)) — edit pins, counts, or colour order on a running device and the next frame uses them. The loopback self-test transmits on the **first** pin in the list.
- `ledsPerPin` (text, default empty) — comma-separated lights-per-pin, e.g. `100,100,50`, matched to `pins` by position. May be empty or shorter than `pins`; see Buffer slicing above.
- `loopbackTxPin` (uint16_t, default unset) — optional **TX override** for the self-test: when set, the loopback transmits on this pin instead of the first pin in `pins`, so the test can run on a dedicated jumper without re-typing the operational `pins`. Falls back to `pins[0]` when unset. Test-only — normal output always uses `pins`. Shown only while `loopbackTest` is on.
- `loopbackRxPin` (uint16_t, default unset) — the RX pin for the loopback self-test; set it when you wire the jumper (the bench used 5). Jumper it to the TX pin (`loopbackTxPin` if set, else the **first** pin in `pins`) to run the test. Shown only while `loopbackTest` is on.
- `loopbackTest` (bool) — a persistent on/off mode for the RMT TX→RX loopback self-test (see Self-test below). While it is on, the test re-runs whenever a relevant control changes (`pins`, `loopbackTxPin`, `loopbackRxPin`, `loopbackFrame`), so the pins can be set in any order and the result always reflects the current wiring; the verdict lands in the module's status field. Turning it off clears the verdict.
- `loopbackFrame` (bool) — whole-frame variant of the self-test, shown only while `loopbackTest` is on. Instead of a 24-bit burst it transmits a real frame (the first pin's slice, or 64 lights) back to back and bit-verifies the entire capture. This is what catches frame-rate corruption and RF interference on the data line — a 24-bit burst can pass through a wire that mangles a sustained frame. On failure the status names the first corrupted bit and light.

## Cross-domain wiring

The driver is added as a child of the `Drivers` container at runtime via the catalog (`POST /api/modules`, a board's [`boards.json`](../../../install/boards.json) `modules` entry) — not boot-wired, exactly like [NetworkSendDriver](NetworkSendDriver.md). RMT is the default LED driver for classic ESP32 and S3 board entries. The type is registered on every target; on a chip without RMT TX channels it is inert. Once added, it receives `setSourceBuffer` / `setCorrection` / `setLayer` from `Drivers::passBufferToDrivers` (which wires every child, boot- or runtime-added), and applies the same `const Correction*` ArtNet uses. The **symbol encode** (`encodeWs2812Symbols` in `RmtSymbol.h`) is domain code in `src/light/` so it is host-testable; the **peripheral** (`platform::rmtWs2812*` in `src/platform/esp32/platform_esp32_rmt.cpp`) is the only ESP-IDF-touching part. Per-chip channel and memory limits come from the IDF SOC capability macros, so the same code serves classic, S3 and P4.

The peripheral half uses the [**modern RMT driver**](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/rmt.html) (ESP-IDF 5.x+ "RMT v2": `driver/rmt_tx.h` / `rmt_rx.h` / `rmt_encoder.h` — `rmt_new_tx_channel()`, a copy encoder, `rmt_transmit()`), **not** the legacy channel-numbered API (`driver/rmt.h`, `rmt_config_t`, `RMT_CHANNEL_n`, `rmt_write_items()`). This isn't a preference — the legacy driver was **removed entirely in [ESP-IDF v6](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32/migration-guides/release-6.x/6.0/peripherals.html)** (the build IDF), so the modern API is the only one that exists. One payoff is portability: the same v2 code serves every RMT-bearing target with no per-chip branching, including the [**P4**](https://www.espressif.com/en/products/socs/esp32-p4), whose RMT additionally has a DMA backend (`SOC_RMT_SUPPORT_DMA`, used by the whole-frame loopback capture — the classic ESP32 has no RMT DMA).

## Loopback self-test (on device)

The RMT peripheral is a transceiver, so the driver can verify its own output on real silicon — no separate test firmware. Jumper the **first** pin in `pins` (TX) to `loopbackRxPin`, then tick the `loopbackTest` control: the driver transmits a known WS2812 pattern out the data pin, captures it back on the RX pin, decodes, and compares. To test another output, temporarily move it to the front of the list. The outcome goes to the module's **status field** (`setStatus`): `loopback PASS`, `loopback FAIL: sent … got …`, or `loopback: jumper not detected` (a plain-GPIO continuity pre-check runs first, so a wiring fault is reported as such, not mistaken for a code bug). The test releases **all** TX channels first (so the RX capture can always allocate RMT memory, even with every channel in use) and briefly drives the test pattern, so any strips flicker once during the run; normal output resumes after. All hardware lives in `platform::rmtWs2812Loopback`.

The default test sends a 24-bit pattern — enough to prove the GPIO emits correct bytes, but blind to faults that only appear over a sustained transfer (frame-rate DMA corruption, RF interference on a long data line — the *intermittent flicker* class of bug). Tick `loopbackFrame` to switch to the whole-frame variant: it transmits a real frame the size of the first pin's slice, back to back like the render loop, captures the **entire** frame, and bit-verifies every WS2812 bit. A single flipped bit anywhere fails the test and the status reports its position (`loopback FAIL: bad bit N/M (light K)`); a clean run reports the bit count (`loopback PASS (M bits)`). Run it while WiFi is active to reproduce interference that only manifests under radio load. Hardware lives in `platform::rmtWs2812LoopbackFrame`. (On the classic ESP32, which has no RMT DMA, the whole-frame capture is capped to one RMT channel's worth of symbols — ~2 RGB lights — and the frame is still clocked back to back; the S3/P4 capture the full frame via DMA.)

## Troubleshooting: flicker on LEDs that should be off

Random wrong colours on LEDs that the effect leaves black — most often a few stray pixels flickering — is, on a 3.3 V ESP32 driving WS2812 **directly**, almost always a **data-line signal-integrity** problem, not a firmware bug. WS2812 wants a logic-high near 0.7 × VDD (≈ 3.5 V on a 5 V strip), but the ESP32 only drives 3.3 V, so individual bits sit at the margin and noise tips them. Confirm the firmware is innocent before reaching for the soldering iron — these checks were the actual diagnosis path on the bench (recorded in [decisions.md](../../../history/decisions.md)):

1. **Is the data clean?** The preview/source buffer is the logical RGB the effect produced — if it shows no stray colour, the effect is innocent (the corruption is downstream of the buffer).
2. **Is the firmware/peripheral clean?** Run the `loopbackFrame` self-test through a short jumper on the data pin. A `PASS` means the RMT encode + transmit emit bit-perfect WS2812 — the GPIO is fine.
3. **Is it WiFi RF?** Lower `Network.txPowerSetting` from 20 dBm down toward 2 and watch. If the flicker shrinks with TX power, it's radio coupling into the data wire (mitigate with the cap below). If it's **unchanged across the whole sweep, it is not the radio** — it's the physical data path.

When 1–3 all come back clean, the fix is electrical, in rough order of effectiveness:

- **Add a 3.3 → 5 V level shifter** on the data line (e.g. 74HCT125 / 74AHCT125) — the single most effective fix; it restores the logic-high margin the LEDs expect.
- **Add a ~330 Ω series resistor** at the GPIO, close to the board, to damp reflections.
- **Shorten / shield the data wire**, and keep it away from the power leads and the antenna.
- **Share a solid, thick common ground** between the strip's supply and the board.
- If RF coupling was implicated by step 3, set a per-board `Network.txPowerSetting` cap (the same `boards.json` mechanism the ESP32-S3 N16R8 Dev uses).

## Tests

Full case list in the generated [unit tests § RmtLedDriver](../../../tests/unit-tests.md#rmtleddriver) (regenerated from the test files, never drifts). What's covered:

- **Encoder (CI, host):** the bit→symbol contract — MSB-first, exact T0H/T1H tick widths, GRB ordering via Correction, RGBW → 32 symbols/light — with no hardware; written red before the encoder, pins it now.
- **Lifecycle (CI, host):** the symbol-buffer ownership — sized in `onBuildState`, survives a rebuild (reinit must not free it), freed on teardown — the class of bug that once reached hardware, now caught on every push.
- **Pins (CI, host):** the `pins`/`ledsPerPin` parsing (bad tokens, duplicates, chip limit) and slice arithmetic (explicit counts, even-split remainder, clamping) down to the per-pin symbol offsets, plus the empty-default idle (an unconfigured driver claims no GPIO).
- **`loopbackTxPin` control (CI, host):** the conditional control — bound always (so persistence can load it), shown only while `loopbackTest` is on.
- **Hardware:** the driver is catalog-added (not boot-wired) — verified on the S3 and classic ESP32 bench: a fresh-erased board has no `RmtLed` until a board is selected, a catalog inject creates it under `Drivers` with its pins, and it persists across a reboot. The `loopbackTxPin` override is set distinct from the operational `pins` (S3: `pins`=18, `loopbackTxPin`=13; classic: `pins`=18, `loopbackTxPin`=4), so flipping `loopbackTest` runs the self-test on the jumper pin without retyping `pins`.

## Prior art

The WS2812 protocol fundamentals and the RMT-first / loopback-test strategy come from the project's [LED driver analysis](../../../backlog/leddriver-analysis-top-down.md), which studies FastLED's `clockless_rmt_esp32`, hpwit's I2S drivers, and WLED — read for the lessons, not copied. FastLED's manual ping-pong refill (their "RMT5" worker, distinct from the IDF *driver* version above) is what makes their path more WiFi-resilient than a naive DMA-less refill ([FastLED #2082](https://github.com/FastLED/FastLED/issues/2082)); we sidestep that whole class of refill deadlines differently — by pre-encoding the entire frame and letting the modern driver stream it, so there is no per-frame refill to miss. Per-output (pin, count) rows are the WLED LED-settings pattern.

## Source

[RmtLedDriver.h](../../../../src/light/drivers/RmtLedDriver.h)
