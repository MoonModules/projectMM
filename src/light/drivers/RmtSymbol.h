#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

// Encode wire-ordered LED bytes into ESP32 RMT symbols — domain logic, no ESP
// header, so it is host-testable without an ESP32 (the platform owns only the
// peripheral that consumes these symbols; see platform.h rmtWs2812*).
//
// RMT symbol layout (matches ESP-IDF's rmt_symbol_word_t, documented here so no
// driver/rmt_*.h leaks into src/light/): one 32-bit word is two 16-bit halves,
// each a (duration:15, level:1) pair with the level in bit 15:
//
//   bits  0..14 : duration0 (RMT ticks)      bit 15 : level0 (0 or 1)
//   bits 16..30 : duration1 (RMT ticks)      bit 31 : level1 (0 or 1)
//
// One WS2812 data bit = one symbol: HIGH for t?hTicks, then LOW for the rest of
// the cell. So half0 = (t?hTicks, level 1), half1 = (period - t?h, level 0).
constexpr uint32_t makeRmtSymbol(uint16_t dur0, uint8_t lvl0,
                                 uint16_t dur1, uint8_t lvl1) {
    return (static_cast<uint32_t>(dur0 & 0x7FFF))
         | (static_cast<uint32_t>(lvl0 & 1) << 15)
         | (static_cast<uint32_t>(dur1 & 0x7FFF) << 16)
         | (static_cast<uint32_t>(lvl1 & 1) << 31);
}

// Encode one light's already-wire-ordered bytes (`channels` of them — brightness,
// GRB reorder and any RGBW white have ALREADY been applied by Correction) into
// `channels * 8` RMT symbols at `out`, MSB-first within each byte. Each data bit
// becomes one symbol: HIGH for t1hTicks (a 1) or t0hTicks (a 0), then LOW for
// (periodTicks - that high time). Durations are in RMT ticks (the caller converts
// ns→ticks from the peripheral's granted resolution). `out` must hold at least
// channels*8 words.
//
// Header-only inline (light domain is header-only; see coding-standards.md). The
// host encoder test asserts this contract: GRB order via the corrected input,
// MSB-first, exact high/low ticks per bit. Implemented in Phase C.
inline void encodeWs2812Symbols(const uint8_t* wire, uint8_t channels,
                                uint16_t t0hTicks, uint16_t t1hTicks,
                                uint16_t periodTicks, uint32_t* out) {
    const uint16_t t0Low = static_cast<uint16_t>(periodTicks - t0hTicks);
    const uint16_t t1Low = static_cast<uint16_t>(periodTicks - t1hTicks);
    // Pre-build the two possible symbols once; each data bit picks one.
    const uint32_t sym0 = makeRmtSymbol(t0hTicks, 1, t0Low, 0);
    const uint32_t sym1 = makeRmtSymbol(t1hTicks, 1, t1Low, 0);
    size_t s = 0;
    for (uint8_t ch = 0; ch < channels; ch++) {
        const uint8_t byte = wire[ch];
        for (int bit = 7; bit >= 0; bit--) {   // MSB-first within the byte
            out[s++] = (byte & (1u << bit)) ? sym1 : sym0;
        }
    }
}

} // namespace mm
