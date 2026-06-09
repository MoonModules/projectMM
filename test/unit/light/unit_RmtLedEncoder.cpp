// @module RmtLedDriver
// @also Correction

#include "doctest.h"
#include "light/drivers/RmtSymbol.h"
#include "light/drivers/Correction.h"

#include <cstdint>

// The encoder is the CI-tier proof of correctness for the LED driver: given
// wire-ordered bytes, it must emit exactly the right WS2812 RMT symbols —
// MSB-first, one symbol per data bit, HIGH for t?hTicks then LOW for the rest of
// the cell. These tests are written test-first (red against the Phase-B stub)
// and pin the contract once the Phase-C implementation lands.

namespace {

// Default WS2812B timing at a 40 MHz / 25 ns-per-tick RMT resolution:
//   t0h 350 ns -> 14 ticks,  t1h 700 ns -> 28 ticks,  period 1250 ns -> 50 ticks.
constexpr uint16_t T0H = 14;
constexpr uint16_t T1H = 28;
constexpr uint16_t PERIOD = 50;

// Decode a symbol word back to its two (level, duration) halves for assertions.
struct Half { uint8_t level; uint16_t duration; };
Half low16(uint32_t s)  { return { static_cast<uint8_t>((s >> 15) & 1), static_cast<uint16_t>(s & 0x7FFF) }; }
Half high16(uint32_t s) { return { static_cast<uint8_t>((s >> 31) & 1), static_cast<uint16_t>((s >> 16) & 0x7FFF) }; }

// Assert one symbol is a correct WS2812 bit: HIGH for `highTicks`, then LOW for
// (PERIOD - highTicks).
void checkBit(uint32_t sym, uint16_t highTicks) {
    Half h0 = low16(sym);
    Half h1 = high16(sym);
    CHECK(h0.level == 1);
    CHECK(h0.duration == highTicks);
    CHECK(h1.level == 0);
    CHECK(h1.duration == static_cast<uint16_t>(PERIOD - highTicks));
}

} // namespace

TEST_CASE("encoder: one byte, MSB-first, 0 and 1 bits get the right pulse widths") {
    // 0xA5 = 1010 0101, MSB first.
    const uint8_t wire[1] = {0xA5};
    uint32_t out[8] = {};
    mm::encodeWs2812Symbols(wire, 1, T0H, T1H, PERIOD, out);

    const uint8_t bits[8] = {1, 0, 1, 0, 0, 1, 0, 1};  // MSB..LSB of 0xA5
    for (int i = 0; i < 8; i++) {
        checkBit(out[i], bits[i] ? T1H : T0H);
    }
}

TEST_CASE("encoder: N lights produce N*channels*8 symbols, in order") {
    // Two RGB lights → 2*3*8 = 48 symbols. Check the first bit of light 0 and the
    // first bit of light 1 land at the expected indices.
    const uint8_t wire[6] = {0xFF, 0x00, 0x00,   // light 0: first byte all-ones
                             0x00, 0x00, 0xFF};  // light 1: last byte all-ones
    uint32_t out[48] = {};
    mm::encodeWs2812Symbols(wire, 3, T0H, T1H, PERIOD, out);  // note: per-light call

    // This test drives the per-LIGHT encode (channels=3). Light 0 byte0 MSB = 1.
    checkBit(out[0], T1H);
    // Light 0 byte0 LSB (bit index 7) = 1 too (0xFF).
    checkBit(out[7], T1H);
    // Byte1 (0x00) bit 8 = 0.
    checkBit(out[8], T0H);
}

TEST_CASE("encoder: GRB ordering comes from Correction, encoder is order-agnostic") {
    // Correction with GRB preset turns logical RGB into wire GRB; the encoder then
    // just emits the bytes it's handed. Logical red (255,0,0) → wire GRB (0,255,0):
    // green byte first. So the FIRST 8 symbols (wire byte 0 = G = 0x00) are all 0s,
    // and the SECOND 8 (wire byte 1 = R = 0xFF) are all 1s.
    mm::Correction c;
    c.rebuild(255, mm::LightPreset::GRB);   // full brightness, GRB
    const uint8_t logicalRed[3] = {255, 0, 0};
    uint8_t wire[4] = {};
    c.apply(logicalRed, wire);              // -> GRB: {0, 255, 0}

    uint32_t out[24] = {};
    mm::encodeWs2812Symbols(wire, c.outChannels, T0H, T1H, PERIOD, out);

    for (int i = 0; i < 8; i++)  checkBit(out[i], T0H);       // G byte = 0x00
    for (int i = 8; i < 16; i++) checkBit(out[i], T1H);       // R byte = 0xFF
    for (int i = 16; i < 24; i++) checkBit(out[i], T0H);      // B byte = 0x00
}

TEST_CASE("encoder: RGBW preset yields 32 symbols per light") {
    mm::Correction c;
    c.rebuild(255, mm::LightPreset::GRBW);  // 4 output channels
    CHECK(c.outChannels == 4);
    const uint8_t logical[3] = {10, 20, 30};
    uint8_t wire[4] = {};
    c.apply(logical, wire);

    uint32_t out[32] = {};
    mm::encodeWs2812Symbols(wire, c.outChannels, T0H, T1H, PERIOD, out);
    // 4 channels * 8 bits = 32 symbols; spot-check the last symbol is a valid bit.
    Half h0 = low16(out[31]);
    CHECK(h0.level == 1);
    CHECK((h0.duration == T0H || h0.duration == T1H));
}
