#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared CRTP body
#include "platform/platform.h"

#include <cstdint>

namespace mm {

// WS2812B output over the ESP32-P4 Parlio (Parallel IO) TX peripheral: up to 8
// strands clock out SIMULTANEOUSLY, one GPIO lane each, fed consecutive slices of
// the source buffer. The P4's scale path, sibling of LcdLedDriver.
//
// The whole body lives in ParallelLedDriver (shared with the i80 driver) — same
// pins/ledsPerPin controls, the SAME per-ROW encoder (a Parlio bus byte and an
// i80 bus byte are identical: one word per slot, bit L = data line L), the same
// fused lifecycle, latch pad, and single-shot autonomous-DMA transfer. This class
// supplies only the Parlio-specific pieces, and Parlio is the SIMPLER peripheral,
// so it supplies less than the i80 driver:
//   - NO clockPin/dcPin: Parlio generates the pixel clock itself (kClockHz),
//     so there are no sacrificial WR/DC lines (addBusControls is empty, the extra-
//     pin tracking is a no-op).
//   - kExactLaneCount = false: i80 rejects a partial bus, Parlio runs on 1..8
//     lanes — whatever `pins` names.
// Prior art: the ESP32-P4 Parlio peripheral, the hpwit/FastLED parallel-WS2812
// lineage — architecture studied, never copied (see ParlioLedDriver.md).
class ParlioLedDriver : public ParallelLedDriver<ParlioLedDriver> {
public:
    // All controls default to UNSET — pins="", ledsPerPin="" (= all lights on the
    // first lane, even-split with one lane), loopbackRxPin=0 — so no constructor is
    // needed (the base zero-initialises them). Pins/loopback are unset because the
    // strand is user-soldered: a hard-coded pin would guess the user's wiring and
    // could drive a pin committed elsewhere ("default only when it cannot do harm",
    // see decisions.md). The P4-NANO bench used pins "20,21,22,23,24,25,26,27",
    // loopbackRxPin 33 (clear of the NANO's strapping 34-38, Ethernet RMII
    // 28-31/49-52, C6 SDIO 14-19/54, I2C 7-8 — clear GPIOs are 20-27, 32-33, 39-48).

    // --- CRTP hooks the base calls (all non-virtual; no vtable) ---

    static constexpr uint8_t lanesAvailable() { return platform::parlioLanes; }
    static constexpr bool kExactLaneCount = false;   // 1..8 lanes all valid
    static constexpr const char* kInitFailMsg = "Parlio init failed — check pins / memory";

    // The WS2812 slot rate (375 ns @ 2.67 MHz) — identical to the LCD driver's;
    // the P4 Parlio's 160 MHz PLL clock divides to it exactly (/60).
    static constexpr uint32_t kClockHz = 2'666'666;

    // Parlio has no sacrificial clock/DC pins, so no extra controls and no extra
    // pin tracking (the bus rebuilds on a data-pin change alone).
    void addBusControls() {}
    bool busControlTriggersBuild(const char*) const { return false; }
    void recordBusPins() {}
    bool extraBusPinsCurrent() const { return true; }

    bool busInit(size_t frameBytes) {
        return platform::parlioWs2812Init(parlio_, laneList_, laneCount_,
                                          kClockHz, frameBytes);
    }
    uint8_t* busBuffer()                 { return platform::parlioWs2812Buffer(parlio_); }
    size_t   busCapacity() const         { return platform::parlioWs2812BufferCapacity(parlio_); }
    bool     busTransmit(size_t bytes)   { return platform::parlioWs2812Transmit(parlio_, bytes); }
    void     busWait(uint32_t ms)        { platform::parlioWs2812Wait(parlio_, ms); }
    void     busDeinit()                 { platform::parlioWs2812Deinit(parlio_); }

    // Parlio runs on a single lane, so the loopback builds its own private 1-lane
    // unit on lane 0 (no i80 full-bus workaround needed; no WR/DC to pass).
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) {
        return platform::parlioWs2812Loopback(laneList_, laneCount_, loopbackRxPin,
                                              frame, frameBytes, dataBytes, rowBits);
    }

private:
    platform::ParlioWs2812Handle parlio_;
};

} // namespace mm
