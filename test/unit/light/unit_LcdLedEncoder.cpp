// @module LcdLedDriver
// @also Correction

#include "doctest.h"
#include "light/drivers/Correction.h"
#include "light/drivers/LcdSlots.h"

#include <cstring>

// The success spec for the LCD_CAM 3-slot encode, written RED before the
// encoder exists (the increment-1 methodology): a known wire row + lane mask
// → the exact slot-byte stream. Pins the transpose (one bus byte carries one
// bit of every lane), MSB-first bit order, and the lanes-active-mask rule
// that keeps short strands idle-LOW.

namespace {

// out holds channels*8 triplets of (slot0, slot1, slot2).
struct Slots {
    uint8_t bytes[4 * 8 * 3];   // up to 4 channels
    const uint8_t* triplet(int bit) const { return bytes + bit * 3; }
};

} // namespace

// One lane, one byte 0xA5: slot0 always the mask, slot1 follows the bits MSB-first, slot2 always zero.
TEST_CASE("LCD encoder: one lane, MSB-first, 3 slots per bit") {
    uint8_t wire[8 * 4] = {};
    wire[0] = 0xA5;   // lane 0, channel 0: 1010 0101
    Slots s{};
    mm::encodeWs2812LcdSlots(wire, 0x01, 1, s.bytes);

    const uint8_t expectBits[8] = {1, 0, 1, 0, 0, 1, 0, 1};
    for (int bit = 0; bit < 8; bit++) {
        const uint8_t* t = s.triplet(bit);
        CHECK(t[0] == 0x01);                          // pulse start: active lanes HIGH
        CHECK(t[1] == (expectBits[bit] ? 0x01 : 0x00)); // data slot
        CHECK(t[2] == 0x00);                          // pulse tail: all LOW
    }
}

// Two lanes 0xFF/0x00 in one row: the data slot carries lane 0's bit only — the transpose itself.
TEST_CASE("LCD encoder: transpose across two lanes") {
    uint8_t wire[8 * 4] = {};
    wire[0 * 4 + 0] = 0xFF;   // lane 0: all ones
    wire[1 * 4 + 0] = 0x00;   // lane 1: all zeros
    Slots s{};
    mm::encodeWs2812LcdSlots(wire, 0x03, 1, s.bytes);

    for (int bit = 0; bit < 8; bit++) {
        const uint8_t* t = s.triplet(bit);
        CHECK(t[0] == 0x03);   // both lanes pulse-start HIGH
        CHECK(t[1] == 0x01);   // only lane 0 carries a 1
        CHECK(t[2] == 0x00);
    }
}

// A lane excluded from the mask contributes to NEITHER slot 0 nor slot 1, even with garbage wire bytes — short strands idle LOW (no white flashes).
TEST_CASE("LCD encoder: inactive lanes stay LOW regardless of wire content") {
    uint8_t wire[8 * 4];
    std::memset(wire, 0xFF, sizeof(wire));   // garbage everywhere
    Slots s{};
    mm::encodeWs2812LcdSlots(wire, 0x01, 1, s.bytes);   // only lane 0 active

    for (int bit = 0; bit < 8; bit++) {
        const uint8_t* t = s.triplet(bit);
        CHECK(t[0] == 0x01);   // lane 1..7 absent from the pulse start
        CHECK(t[1] == 0x01);   // and from the data slot
        CHECK(t[2] == 0x00);
    }
}

// Mask 0 (a row past every lane's strand) is a fully idle row.
TEST_CASE("LCD encoder: empty mask emits all-zero slots") {
    uint8_t wire[8 * 4];
    std::memset(wire, 0xFF, sizeof(wire));
    Slots s{};
    std::memset(s.bytes, 0xEE, sizeof(s.bytes));
    mm::encodeWs2812LcdSlots(wire, 0x00, 1, s.bytes);
    for (int i = 0; i < 8 * 3; i++) CHECK(s.bytes[i] == 0x00);
}

// Channel order comes from Correction (logical red → GRB wire {0,255,0}); the encoder is order-agnostic.
TEST_CASE("LCD encoder: GRB ordering via Correction") {
    mm::Correction corr;
    corr.rebuild(255, mm::LightPreset::GRB);
    const uint8_t rgb[3] = {255, 0, 0};   // logical red
    uint8_t wire[8 * 4] = {};
    corr.apply(rgb, wire);                 // lane 0 wire = {0, 255, 0}

    Slots s{};
    mm::encodeWs2812LcdSlots(wire, 0x01, 3, s.bytes);
    for (int bit = 0; bit < 8; bit++) {
        CHECK(s.triplet(bit)[1] == 0x00);        // G byte: all zero data
        CHECK(s.triplet(8 + bit)[1] == 0x01);    // R byte: all ones data
        CHECK(s.triplet(16 + bit)[1] == 0x00);   // B byte: all zero data
    }
}

// RGBW rows emit 4 channels × 8 bits × 3 slots = 96 bytes.
TEST_CASE("LCD encoder: RGBW row is 96 slot bytes") {
    mm::Correction corr;
    corr.rebuild(255, mm::LightPreset::GRBW);
    const uint8_t rgb[3] = {10, 10, 10};
    uint8_t wire[8 * 4] = {};
    corr.apply(rgb, wire);

    uint8_t out[4 * 8 * 3];
    std::memset(out, 0xEE, sizeof(out));
    mm::encodeWs2812LcdSlots(wire, 0x01, 4, out);
    // The last triplet was written (its tail slot is 0, not the 0xEE poison).
    CHECK(out[4 * 8 * 3 - 1] == 0x00);
}
