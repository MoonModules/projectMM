#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared CRTP body
#include "platform/platform.h"

#include <cstdint>
#include <cstring>  // std::strcmp

namespace mm {

// WS2812B output over the ESP32-S3 LCD_CAM i80 bus: up to 8 strands clock out
// SIMULTANEOUSLY, one GPIO lane each, fed consecutive slices of the source
// buffer. The S3's scale path — RMT gives it 4 channels, this gives 8 lanes for
// the wall time of one.
//
// The whole body — pins/ledsPerPin controls, the per-ROW fused correct+encode
// (LcdSlots.h), the fused reinit (the i80 bus owns the DMA buffer and its max
// transfer size is fixed at creation, so re-creating the bus IS the buffer
// resize), the latch pad, the loopback self-test — lives in ParallelLedDriver,
// shared with ParlioLedDriver. This class supplies only the i80-specific pieces:
// the sacrificial WR (pixel clock) + DC GPIOs the peripheral mandates even though
// WS2812 ignores both, the "exactly 8 pins" rule (the i80 layer rejects a partial
// bus), and the platform::lcdWs2812* calls.
//
// The whole frame (plus a >=300 µs zeroed latch pad) is pre-encoded off any ISR
// path, then one autonomous GDMA transfer ships it — no refill deadlines, so the
// WiFi-induced bit-slip of refill-based drivers can't occur. Prior art: Adafruit's
// LCD_CAM discovery, hpwit's I2SClockless lineage, FastLED's S3 driver —
// architecture studied, never copied (see LcdLedDriver.md).
class LcdLedDriver : public ParallelLedDriver<LcdLedDriver> {
public:
    // Data pins + loopback pin default to UNSET: they are user-soldered (the strand
    // runs to whatever GPIOs the user wired), so a hard-coded default would be a
    // guess that could drive a pin the user committed elsewhere — empty until set,
    // the driver idles meanwhile (the "default only when it cannot do harm" rule;
    // see decisions.md). The ESP32-S3 N16R8 Dev bench wiring was pins "1,2,4,5,6,7,8,9",
    // loopbackRxPin 12 (kept clear of the octal-PSRAM pins 26-37, USB 19/20, and
    // strapping pins) — set those again to reproduce the bench. (Base declares
    // pins="" and loopbackRxPin=0, so the empty default needs no code here.)

    // WR (pixel clock) and DC are different: the IDF i80 bus *requires* both on real
    // GPIOs (esp_lcd_panel_io_i80.c: `wr_gpio_num >= 0 && dc_gpio_num >= 0`), yet the
    // WS2812 strands ignore both — they are peripheral-fixed, not user-strand wiring,
    // so a sensible overridable default cannot do harm (same class as the chip-fixed
    // Ethernet pins). The data pins above gate startup, so the bus stays idle until
    // the user sets them regardless. (Dropping WR/DC entirely needs a direct-LCD_CAM
    // driver that bypasses esp_lcd, hpwit-style — backlogged, not this increment.)
    uint16_t clockPin = 10;
    uint16_t dcPin = 11;

    // --- CRTP hooks the base calls (all non-virtual; no vtable) ---

    static constexpr uint8_t lanesAvailable() { return platform::lcdLanes; }
    static constexpr bool kExactLaneCount = true;   // i80 needs all 8 data lanes
    static constexpr const char* kInitFailMsg = "LCD init failed — check pins / memory";

    void addBusControls() {
        controls_.addUint16("clockPin", clockPin);
        controls_.addUint16("dcPin", dcPin);
    }
    bool busControlTriggersBuild(const char* name) const {
        return std::strcmp(name, "clockPin") == 0 || std::strcmp(name, "dcPin") == 0;
    }

    bool busInit(size_t frameBytes) {
        return platform::lcdWs2812Init(lcd_, laneList_, laneCount_,
                                       clockPin, dcPin, frameBytes);
    }
    uint8_t* busBuffer()                 { return platform::lcdWs2812Buffer(lcd_); }
    size_t   busCapacity() const         { return platform::lcdWs2812BufferCapacity(lcd_); }
    bool     busTransmit(size_t bytes)   { return platform::lcdWs2812Transmit(lcd_, bytes); }
    void     busWait(uint32_t ms)        { platform::lcdWs2812Wait(lcd_, ms); }
    void     busDeinit()                 { platform::lcdWs2812Deinit(lcd_); }

    // The i80 layer requires all 8 data GPIOs valid, so a 1-lane private bus is
    // impossible; the loopback builds the full-width bus and carries the pattern
    // on lane 0. Passes the WR/DC pins the init needs.
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) {
        return platform::lcdWs2812Loopback(laneList_, laneCount_, clockPin, dcPin,
                                           loopbackRxPin, frame, frameBytes,
                                           dataBytes, rowBits);
    }

    // Store WR/DC alongside the data pins, so a clockPin/dcPin edit rebuilds the
    // bus too (not just a data-pin change).
    void recordBusPins() { lastClockPin_ = clockPin; lastDcPin_ = dcPin; }
    bool extraBusPinsCurrent() const {
        return lastClockPin_ == clockPin && lastDcPin_ == dcPin;
    }

private:
    platform::LcdWs2812Handle lcd_;
    uint16_t lastClockPin_ = 0;
    uint16_t lastDcPin_ = 0;
};

} // namespace mm
