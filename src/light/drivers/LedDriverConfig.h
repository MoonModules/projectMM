#pragma once

#include <cstdint>

namespace mm {

inline constexpr uint8_t kLedChipsetWs2812b = 0;
inline constexpr uint8_t kLedChipsetSk6812Rgbw = 1;
inline constexpr const char* kLedChipsetOptions[] = {"WS2812B", "SK6812 RGBW"};
inline constexpr uint8_t kLedChipsetCount =
    sizeof(kLedChipsetOptions) / sizeof(kLedChipsetOptions[0]);

// Wire timing for a clockless addressable-LED chipset (WS2812B by default).
// Pure data — no platform include — so the encoder that reads it is host-testable.
//
// WS2812-class chips are a 1-wire NRZ protocol at 800 kHz: every bit is a
// `period_ns` cell that starts HIGH and drops LOW, where the HIGH duration
// encodes 0 vs 1. There is no clock line. The defaults below target the
// reverse-engineered ~600 ns decode threshold (see leddriver-analysis-top-down.md
// §1.1), so they satisfy WS2812, WS2812B and SK6812 at once.
struct LedDriverConfig {
    uint32_t t0h_ns    = 350;    // "0" bit: HIGH for this long, then LOW for the rest
    uint32_t t1h_ns    = 700;    // "1" bit: HIGH for this long, then LOW for the rest
    uint32_t period_ns = 1250;   // full bit cell (t?h + the trailing LOW)
    uint32_t reset_us  = 300;    // idle-LOW latch between frames (>= 300 us, current silicon)
    bool     invert    = false;  // flip output polarity, for inverting level-shifters

    void applyChipset(uint8_t chipset) {
        const bool keepInvert = invert;
        switch (chipset) {
            case kLedChipsetSk6812Rgbw:
                t0h_ns = 300;
                t1h_ns = 600;
                period_ns = 1250;
                reset_us = 300;
                break;
            case kLedChipsetWs2812b:
            default:
                t0h_ns = 350;
                t1h_ns = 700;
                period_ns = 1250;
                reset_us = 300;
                break;
        }
        invert = keepInvert;
    }
};

} // namespace mm
