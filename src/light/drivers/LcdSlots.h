#pragma once

#include <cstdint>

namespace mm {

// WS2812 encode for parallel WS2812 buses — the contract between a parallel
// driver (domain) and a parallel peripheral, named for the wire unit it builds
// (one pixel-clock SLOT = one byte on the 8-bit bus), the RmtSymbol.h sibling.
// Used by BOTH the LCD_CAM i80 driver (ESP32-S3, LcdLedDriver) and the Parlio
// driver (ESP32-P4, ParlioLedDriver) — a Parlio bus byte and an i80 bus byte
// are identical (one word per slot, bit L = data line L), so one encoder
// serves both. Pure data transform, no platform include — the host CI encoder
// test (unit_LcdLedEncoder.cpp) pins it with no ESP32.
//
// Technique (hpwit / Adafruit "ESP32uesday" / FastLED S3 lineage — studied,
// not copied): every WS2812 data bit becomes THREE bus slots clocked at
// 2.67 MHz (slot = 375 ns, bit = 1.125 µs):
//
//   slot 0: activeMask        — every active lane HIGH (the pulse start)
//   slot 1: data bits & mask  — lane L's current bit at bus bit L
//   slot 2: 0x00              — every lane LOW (the pulse tail)
//
// so a "1" bit is HIGH for 2 slots (750 ns ≈ t1h 700 ns) and a "0" bit for
// 1 slot (375 ns ≈ t0h 350 ns). The LedDriverConfig nanosecond fields are
// APPROXIMATED by the slot clock — timing is fixed by the pclk (chosen in
// platform_esp32_lcd.cpp; 375 ns keeps T0H inside even the newest WS2812B
// revisions' ~380 ns max — longer "0" pulses wash strips out white on a
// direct 3.3 V data line).
//
// Lanes-active-mask rule: a lane whose strand is shorter than the longest one
// must appear in NEITHER slot 0 nor slot 1 once its lights are exhausted —
// excluded lanes idle LOW for the rest of the frame instead of flashing white.
// The caller expresses that by clearing the lane's bit in `activeMask`.
//
// Bus bit L = the L-th entry of the driver's `pins` list (D0 = first pin).
// Bits go MSB-first per byte; channel order (GRB, …) is already applied by
// Correction before the encode, so the encoder is order-agnostic (same
// contract as encodeWs2812Symbols).

// Encode one ROW (the same light index across all lanes) into 3-slot bytes.
//   wire:       kMaxLanes × 4 corrected wire bytes, lane-major
//               (wire[lane * 4 + channel]); only lanes set in activeMask are
//               read — inactive lanes may hold garbage.
//   activeMask: bit L set = lane L drives this row.
//   channels:   wire bytes per light (3 RGB / 4 RGBW).
//   out:        channels * 8 * 3 bytes, fully written.
inline void encodeWs2812LcdSlots(const uint8_t* wire, uint8_t activeMask,
                                 uint8_t channels, uint8_t* out) {
    for (uint8_t ch = 0; ch < channels; ch++) {
        for (int bit = 7; bit >= 0; bit--) {
            uint8_t data = 0;
            for (uint8_t lane = 0; lane < 8; lane++) {
                if (!(activeMask & (1u << lane))) continue;   // inactive: idle LOW
                data |= static_cast<uint8_t>(((wire[lane * 4 + ch] >> bit) & 1u) << lane);
            }
            *out++ = activeMask;   // slot 0: pulse start
            *out++ = data;         // slot 1: the bits
            *out++ = 0x00;         // slot 2: pulse tail
        }
    }
}

} // namespace mm
