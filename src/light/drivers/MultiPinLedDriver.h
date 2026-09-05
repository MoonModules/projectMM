#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared driver body + LedPeripheral
#include "platform/platform.h"


namespace mm {

/// Output driver: parallel 8-or-16-lane WS2812B over the ESP-IDF [esp_lcd i80 bus](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/index.html)
/// — the parallel scale path on **all three i80-capable ESP32 families**. RMT gives a chip 4-8
/// channels; this gives 8-16 lanes for the wall time of one. The magic is that ESP-IDF exposes ONE
/// public i80 API (`esp_i80_new_i80_bus` / `esp_i80_panel_io_tx_color`) and routes it to whichever
/// peripheral the silicon has — so this single backend serves every chip:
///  - **ESP32-S3 / -P4 / -S31:** backed by the dedicated **LCD_CAM** peripheral.
///  - **classic ESP32:** backed by the **I2S peripheral in i80/LCD mode** (the classic has no LCD_CAM;
///    I2S-i80 is its only >8-lane route). IDF's own CMake picks the backend by chip; the two are
///    mutually exclusive per silicon, so `lanesAvailable()` reads whichever lane-count constant is
///    non-zero. Named for the **i80 bus** (the shared API), not a peripheral, since it isn't one
///    peripheral — same reason its sibling backend is named `Parlio` (its own API).
///
/// The shared body (slicing, the whole-frame async double-buffer DMA, the fused encode, the loopback
/// self-test, the `frameTime` KPI) lives in ParallelLedDriver; this backend adds only the i80-specific
/// pieces:
///  - The sacrificial WR (pixel clock) + DC GPIOs the i80 bus mandates even though WS2812 ignores
///    both, and the "exactly 8 or 16 pins" rule (the i80 layer rejects a partial bus). A sub-16 board
///    parks unused lanes + WR/DC on one spare GPIO (the ghost-pin trick).
///  - **The 3-slot-per-bit wire contract:** each WS2812 bit becomes three bus slots at 2.67 MHz
///    (slot = 375 ns): all-active-lanes HIGH, the data bits, then all LOW — so a `1` is HIGH 750 ns
///    and a `0` 375 ns, approximating RMT's 700/350. The slot is deliberately NOT the lineage's
///    ~416 ns: newer WS2812B revisions spec T0H max ≈ 380 ns, and a longer `0` on a direct 3.3 V line
///    gets misread as `1` (the strip washes white). One bus word per slot (bus bit L = the L-th pin);
///    unequal strands idle LOW once exhausted. Slot layout: ParallelSlots.h.
///  - Both silicon paths do **whole-frame chained DMA** (autonomous, CPU out of the timing loop), so the
///    classic I2S path is WiFi-underrun-immune by construction — it does NOT need the ISR-refilled
///    ring / large `nbDmaBuffer` cushion the raw-register I2S-clockless lineage requires.
///  - The platform::i80Ws2812* calls (ESP-IDF's esp_lcd i80 bus + GDMA).
///
/// Prior art: Adafruit's LCD_CAM discovery, hpwit's I2SClockless lineage (classic-ESP32 I2S parallel),
/// FastLED's S3 driver — architecture studied, never copied. We build on IDF's maintained esp_lcd i80
/// abstraction rather than tracing the raw-register I2S driver (*Industry standards, our own code*).
class I80Peripheral : public LedPeripheral {
public:
    // Data pins + loopback pin default to UNSET: they are user-soldered (the strand
    // runs to whatever GPIOs the user wired), so a hard-coded default would be a
    // guess that could drive a pin the user committed elsewhere — empty until set,
    // the driver idles meanwhile (the "default only when it cannot do harm" rule;
    // see lessons.md). The ESP32-S3 N16R8 Dev bench wiring is pins "1,2,4,5,6,7,8,9",
    // loopbackRxPin 12 (kept clear of the octal-PSRAM pins 26-37, USB 19/20, and
    // strapping pins) — set those again to reproduce the bench. (The orchestrator declares
    // pins="" and loopbackRxPin=0, so the empty default needs no code here.)

    /// WR (pixel clock) and DC: the IDF i80 bus *requires* both on real GPIOs
    /// (esp_i80_panel_io_i80.c: `wr_gpio_num >= 0 && dc_gpio_num >= 0`), yet the WS2812 strands
    /// ignore both — they are peripheral-fixed, not user-strand wiring, so a sensible overridable
    /// default cannot do harm (same class as the chip-fixed Ethernet pins). The data pins gate
    /// startup, so the bus stays idle until the user sets them regardless. (Dropping WR/DC entirely
    /// needs a direct-LCD_CAM backend that bypasses esp_lcd, hpwit-style — that is MoonI80Peripheral,
    /// backlogged as this increment's sibling.)
    ///
    /// **`clockPin` is ONE pin doing TWO jobs, and with a 74HCT595 expander the second one is
    /// load-bearing.** WR toggles once per bus word in hardware — which is exactly what a '595's
    /// SRCLK (shift clock) needs — so the same wire serves both: the i80 pixel clock IS the shift
    /// clock. That is the trick that makes the expander cost zero DMA bytes for its clock (hpwit
    /// routes `LCD_PCLK_IDX` straight to the register's clock pin for the same reason).
    ///
    /// Two consequences worth knowing before editing this pin:
    ///  - Every bus word clocks a bit INTO the shift register. That is why the LATCH must ride a
    ///    *data lane* (a bit in every bus word) rather than a second clock output — the peripheral
    ///    only gives us one — and why the latch's word position is so delicate (ParallelSlots.h).
    ///  - In shift mode this pin is wired to the physical '595 clock line on the expander board.
    ///    Changing it means re-wiring hardware, not just re-configuring.
    ///
    /// **Give it a real, free GPIO — do not set -1.** Bench-proven that nothing on a WS2812 strand reads
    /// WR or DC (4096 lights over 16 lanes, and 1440 through a '595, both render with both pins at -1:
    /// the peripheral generates the signals internally and the GPIO matrix only carries them off-chip).
    /// But -1 does not MEAN "unrouted" here — it means **65535**: the value reaches the platform as
    /// `uint16_t`, which slips past IDF's `wr_gpio_num >= 0 && dc_gpio_num >= 0` check
    /// (esp_lcd_panel_io_i80.c), where a properly-typed `GPIO_NUM_NC` would be rejected outright.
    /// `esp_lcd` then hands 65535 to `esp_rom_gpio_connect_out_signal`, and what happens next is PER-TARGET
    /// ROM, not an API contract: the S3 and classic ROMs open with an unsigned bounds compare and return
    /// without writing (a silent no-op), but the **ESP32-P4 ROM has no such guard** and computes a store
    /// ~0x50120554 — a quarter-megabyte past the GPIO block, in another peripheral's window — plus a
    /// >31-bit shift. This backend runs on the P4. IDF's own `esp_rom/patches/esp_rom_gpio.c` is unguarded
    /// too, so the S3's check is an implementation detail a patch could remove, not a promise. FastLED's
    /// LCD_CAM driver parks both pins on a dummy GPIO for the same reason. To spend no GPIO at all, use
    /// MoonI80Peripheral: owning the DMA below esp_lcd, it holds DC at a constant level and routes WR only
    /// when a '595 needs it as SRCLK.
    /// Per-chip, because 10/11 are free GPIOs on the S3 these were chosen on and are the FLASH bus
    /// on a classic ESP32 (6-11): routing the i80 clock onto one wedges the board to a watchdog
    /// reset with no panic and no coredump. `i2sLanes > 0` IS "this is the classic-ESP32 i80" (the
    /// two backends are mutually exclusive per silicon), the same discriminator dmaBudgetBytes()
    /// below keys on, rather than a raw CONFIG_IDF_TARGET that would put chip knowledge outside the
    /// platform layer.
    ///
    /// 18/23 rather than the first free numbers: WR and DC are peripheral-fixed signals no WS2812
    /// strand reads, so this default only has to avoid pins a BOARD is likely to have committed.
    /// 21/22 look free chip-wise and are the QuinLED Dig-Next-2's relay lines, where claiming them
    /// silently switched two power channels off (LEDs dark, no error anywhere). 18/23 are plain
    /// GPIOs on every classic package: no strap, no flash, no UART, and unused by the catalog's
    /// boards. A board that does wire them overrides the control, and reinit() refuses a reserved
    /// pin outright.
    int8_t clockPin = platform::i2sLanes > 0 ? 18 : 10;
    int8_t dcPin    = platform::i2sLanes > 0 ? 23 : 11;

    // --- LedPeripheral descriptors ---

    /// The number of i80 lanes this chip provides (0 = no i80 bus on this chip); the orchestrator's
    /// inert-on-wrong-chip guards key off it. Reads whichever backend the silicon has —
    /// `lcdLanes` (LCD_CAM, S3/P4/S31) or `i2sLanes` (I2S-i80, classic ESP32) — which are mutually
    /// exclusive per chip (at most one is non-zero), so the sum picks the right one.
    uint8_t lanesAvailable() const MM_NONBLOCKING override { return platform::lcdLanes + platform::i2sLanes; }
    bool powerOfTwoBus() const override { return true; }   // the BUS rounds to 8/16; the pin count is free

    /// Whole-frame DMA byte budget. On the classic ESP32 the i80 is the I2S peripheral: its DMA is
    /// INTERNAL-RAM only (no PSRAM) and it holds the whole frame (no streaming ring), so a frame larger
    /// than the free internal DMA block simply cannot allocate — and the failing esp_lcd path can busy-
    /// wait to a watchdog reset. reinit() pre-checks against this and idles with a clear status instead.
    /// Budget = the largest free internal block minus a fixed reserve for the bus descriptors + other
    /// allocations that land between this query and the alloc; sized for ONE frame, since busInit()
    /// downgrades the optional second (doubleBuffer) buffer on its own when only one fits. The classic
    /// path is bounded by construction, so it never returns 0 (which means "no bound"): when the block
    /// is at or under the reserve, it reports a small positive floor so the fit gate still rejects.
    /// On the LCD_CAM chips (S3/P4/S31) the DMA reaches PSRAM → 0 = no bound (the interface default). COLD PATH.
    size_t dmaBudgetBytes() const override {
        if constexpr (platform::i2sLanes > 0) {
            const size_t block = platform::maxInternalAllocBlock();
            constexpr size_t kReserve = 16 * 1024;   // descriptors + headroom for allocs after this query
            constexpr size_t kMinBudget = 1;         // never 0 on the bounded path (0 == "no bound")
            return block > kReserve ? block - kReserve : kMinBudget;
        } else {
            return 0;   // LCD_CAM (S3/P4/S31): PSRAM DMA, no whole-frame ceiling
        }
    }

    // The i80 loopback can't build a 1-lane private bus, so it rebuilds the FULL-WIDTH bus and
    // carries the pattern on lane 0 — the loopback frame must be encoded at the operational bus
    // width (16-bit for a 16-lane driver) to match. (Parlio can do a 1-lane unit, so its backend
    // sets false.)
    bool loopbackFullWidth() const override { return true; }
    /// The classic ESP32's esp_lcd-i80 backend IS the I2S peripheral; on the LCD_CAM chips (S3/P4/S31) it is LcdCam (shared
    /// with MoonI80, which is why the two conflict). Keys on the same i2sLanes flag lanesAvailable does.
    LedHwBlock hwBlock() const override {
        if constexpr (platform::i2sLanes > 0) return LedHwBlock::I2s;
        else return LedHwBlock::LcdCam;
    }
    const char* initFailMsg() const override {
        // Names the same peripheral the label does, so the error and the dropdown agree.
        return (platform::i2sLanes > 0) ? "I2S-IDF: bus init failed, check pins / memory"
                                       : "LCD-IDF: bus init failed, check pins / memory";
    }

    /// Spare bus lanes (shift mode, when the board has fewer data pins than the bus is wide) park on
    /// WR: the peripheral already drives it and the board already wires it, so the lane is inert.
    /// (Overrides the interface default; this is the "ghost pin" the platform layer uses for the same
    /// reason.)
    uint16_t clockPinForBus() const override { return static_cast<uint16_t>(clockPin); }

    /// Bind the i80-specific bus controls: the sacrificial WR (clockPin) and DC pins
    /// the peripheral mandates.
    void addBusControls(ControlList& controls) override {
        controls.addPin("clockPin", clockPin);
        controls.addPin("dcPin", dcPin);
    }
    /// A clockPin or dcPin change triggers a bus rebuild via the prepare sweep.
    bool busControlTriggersBuild(const char* name) const override {
        return std::strcmp(name, "clockPin") == 0 || std::strcmp(name, "dcPin") == 0;
    }

    /// FATAL bus-pin check → routed to the ERROR path (idles the driver), unlike validateBusPins'
    /// per-lane WARNINGS. WR and DC on the SAME GPIO breaks the i80 bus outright (it needs two
    /// distinct control lines — the bus won't init), so it can't be a warn-and-run like a data-lane
    /// collision (which only corrupts that one lane). null = no fatal condition.
    const char* validateBusFatal() const override {
        // An UNSET clockPin/dcPin (-1) is fatal on i80: the bus mandates a valid WR and DC GPIO, but
        // clockPinForBus()/busInit cast the int8_t to uint16_t, so -1 becomes 65535 and slips past
        // IDF's own `wr_gpio_num >= 0 && dc_gpio_num >= 0` guard (see the clockPin doc above). Reject it
        // here, before that cast, so an unconfigured board idles with a clear status instead of an
        // init on a garbage GPIO number.
        if (clockPin < 0) return "clockPin (WR) is unset — the i80 bus needs a valid WR GPIO";
        if (dcPin < 0) return "dcPin is unset — the i80 bus needs a valid DC GPIO";
        if (clockPin == dcPin)
            return "clockPin (WR) and dcPin are the same GPIO — they must differ";
        // Neither may sit on a pin the chip wired to flash or PSRAM: routing I/O there corrupts the
        // device. The driver's own sweep covers the bus LANES, but WR only rides that list when
        // there are spare lanes to park it on (a full-width 8- or 16-pin setup has none) and DC
        // never does, so these two are checked here, where the pair already lives.
        if (platform::gpioCapability(static_cast<uint8_t>(clockPin)).reserved)
            return "clockPin (WR) is wired to flash/PSRAM on this chip - pick another pin";
        if (platform::gpioCapability(static_cast<uint8_t>(dcPin)).reserved)
            return "dcPin is wired to flash/PSRAM on this chip - pick another pin";
        // The '595 latch is a BUS LANE, so it needs its own GPIO: sharing it with WR would make the
        // pixel clock double as the latch (the '595 would present a byte on every shift cycle), and
        // sharing it with DC would latch on the command phase. Both are fatal — the bus builds, but
        // the strands get garbage — so this is an error, not a warning. (Bench-found: WR defaults to
        // GPIO 10, which is the first pin a user reaches for when picking a latch.)
        if (owner_->pinExpanderMode() && owner_->latchPin >= 0) {
            if (owner_->latchPin == clockPin)
                return "latchPin is on clockPin (WR) — the latch needs its own GPIO";
            if (owner_->latchPin == dcPin)
                return "latchPin is on dcPin — the latch needs its own GPIO";
        }
        return nullptr;
    }

    /// Reject a data lane that collides with the WR (clockPin) or DC pin. The i80
    /// peripheral routes a distinct output signal to each of the 8 data lanes plus
    /// WR + DC via the GPIO matrix; IDF does NOT check that they differ, so a data
    /// pin equal to clockPin/dcPin gets two signals on one GPIO and that lane emits
    /// the clock/DC waveform instead of pixel data (silent corruption — the strip on
    /// that lane shows garbage). Fail loud + idle instead, same shape as the other
    /// parse errors.
    // Returns a WARNING string (not an error) if a data lane sits on clockPin (WR) or
    // dcPin: that lane emits the bus-control waveform instead of pixel data. It's a
    // warning because on a board that wires all 8/16 lanes but drives fewer strands,
    // parking WR/DC on an unused data pin is a valid choice — only a lane driving a
    // real strand shows garbage. The orchestrator routes this to setConfigWarn, the driver
    // keeps running. null when the WR/DC pins are clear of the data set.
    const char* validateBusPins(const uint16_t* lanes, uint8_t n) const override {
        for (uint8_t i = 0; i < n; i++) {
            // clockPin/dcPin are int8_t (-1 = unset); only a real GPIO can collide.
            if (clockPin >= 0 && lanes[i] == static_cast<uint16_t>(clockPin))
                return "a LED pin is on clockPin (WR) — that lane carries the clock, not pixels";
            if (dcPin >= 0 && lanes[i] == static_cast<uint16_t>(dcPin))
                return "a LED pin is on dcPin — that lane carries DC, not pixels";
        }
        return nullptr;
    }

    /// Create the i80 bus + its DMA buffer(s) sized for `frameBytes` on the current data lanes plus
    /// the WR/DC pins; `wantSecondBuffer` requests the async double-buffer's second frame buffer
    /// (allocated only if it fits — else single-buffer). Returns whether init succeeded.
    /// **LCD_CAM** is the one silicon path that can host the 74HCT595 expander: it reaches PSRAM (so the
    /// ×8 frame fits), it has no single-transfer cap, and its WR pixel-clock pin IS the shift clock a
    /// '595 needs. The classic ESP32 shares this backend but not that silicon path — its i80 is the I2S
    /// peripheral, whose DMA cannot read PSRAM at all, so a 154 KB frame has nowhere to live. Keying
    /// the flag on `platform::hasLcdCam` makes the refusal a compile-time property of the silicon
    /// rather than a runtime surprise, and the orchestrator then reports it as a config error instead
    /// of letting the bus die at init with "check pins / memory". `hasLcdCam`, not `lcdLanes`: the
    /// two agree on ESP32 (it is defined as `lcdLanes > 0` there) but not on the desktop, which
    /// declares the capability outright so the emulated build can exercise this path.
    bool supportsPinExpander() const override { return platform::hasLcdCam; }

    /// The bus pin list comes from the orchestrator: in shift mode it appends the latch to the data pins
    /// (the latch is a bus lane), so the peripheral drives it. busClockMultiplier() tells the platform
    /// how many bus words one WS2812 slot is shifted out over, so it can scale the pixel clock and the
    /// slot keeps its wire duration.
    bool busInit(size_t frameBytes, bool wantSecondBuffer) override {
        return platform::i80Ws2812Init(i80_, owner_->busPinList(), owner_->busPinCount(),
                                       static_cast<uint16_t>(clockPin),
                                       static_cast<uint16_t>(dcPin), frameBytes, wantSecondBuffer,
                                       owner_->busClockMultiplier());
    }
    /// DMA buffer `i` (0/1) the orchestrator encodes into; buffer 1 is null when the second
    /// buffer didn't fit (single-buffer mode). Both are the same size (busCapacity).
    uint8_t* busBuffer(uint8_t i) override        { return platform::i80Ws2812Buffer(i80_, i); }
    /// The per-buffer byte capacity (fixed at bus creation; both buffers equal).
    size_t   busCapacity() const override         { return platform::i80Ws2812BufferCapacity(i80_); }
    /// Kick off the autonomous transfer of the first `bytes` of DMA buffer `i`;
    /// returns whether it started.
    bool  busTransmit(uint8_t i, size_t bytes) override { return platform::i80Ws2812Transmit(i80_, i, bytes); }
    /// Block up to `ms` for buffer `i`'s in-flight transfer to complete.
    bool  busWait(uint8_t i, uint32_t ms) override      { return platform::i80Ws2812Wait(i80_, i, ms); }
    /// The most recent DMA transfer's wire time (µs) — the WS2812 output floor.
    uint32_t busLastTransmitUs() const override         { return platform::i80Ws2812LastTransmitUs(i80_); }
    /// Tear down the i80 bus and its DMA buffer.
    void     busDeinit() override                 { platform::i80Ws2812Deinit(i80_); }

    /// Run the loopback self-test. The i80 layer requires all 8 data GPIOs valid, so
    /// a 1-lane private bus is impossible; the loopback builds the full-width bus and
    /// carries the pattern on lane 0. Passes the WR/DC pins the init needs.
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) override {
        // The private bus is built from the orchestrator's bus pin list (which appends the latch in
        // shift mode — the latch is a bus lane) and at the shift-mode pclk, so the test transmits
        // exactly what the render loop does. In direct mode both reduce to today's behavior.
        return platform::i80Ws2812Loopback(owner_->busPinList(), owner_->busPinCount(),
                                           static_cast<uint16_t>(clockPin),
                                           static_cast<uint16_t>(dcPin),
                                           static_cast<uint16_t>(owner_->loopbackRxPin),
                                           frame, frameBytes, dataBytes, rowBits,
                                           owner_->busClockMultiplier());
    }

    /// Store WR/DC alongside the data pins, so a clockPin/dcPin edit rebuilds the
    /// bus too (not just a data-pin change).
    void recordBusPins() override { lastClockPin_ = clockPin; lastDcPin_ = dcPin; }
    /// Whether the live bus's WR/DC pins still match the current clockPin/dcPin (so
    /// the orchestrator can skip a rebuild).
    bool extraBusPinsCurrent() const override {
        return lastClockPin_ == clockPin && lastDcPin_ == dcPin;
    }

private:
    platform::I80Ws2812Handle i80_;
    int8_t lastClockPin_ = -1;
    int8_t lastDcPin_ = -1;
};

// Register the esp_lcd-i80 backend into the ParallelLedDriver peripheral registry once, at static-init.
// The label is what the `peripheral` Select shows. Gated by this header's own CONFIG_SOC include in
// main.cpp, so it only registers on i80-capable silicon. There is no separate driver class: the one
// registered ParallelLedDriver drives every backend, selected by the `peripheral` control.
//
// It names the PERIPHERAL, not the bus protocol. "i80" is the Intel 8080 bus shape esp_lcd speaks, and
// it matches nothing a user can look up: on the classic ESP32 this backend IS the I2S peripheral, on
// LCD_CAM chips it is the LCD peripheral (see hwBlock()). So the label follows the silicon — and says
// `-IDF` because this backend drives it through esp_lcd, against the MoonI80 backend's `-MM` (our
// own GDMA layer below esp_lcd). Peripheral + who drives it is the whole choice a user is making.
inline constexpr const char* kI80Label = (platform::i2sLanes > 0) ? "I2S-IDF" : "LCD-IDF";
inline const bool kI80PeripheralRegistered =
    ParallelLedDriver::registerPeripheral(kI80Label, []() -> LedPeripheral* { return new I80Peripheral(); });

} // namespace mm
