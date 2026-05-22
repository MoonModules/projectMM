#pragma once

#include <cstdint>

namespace mm {

struct RGB {
    uint8_t r, g, b;
};

// Integer HSV to RGB conversion. Maps h 0-255 to 6 sectors of 256 steps.
// h: 0-255 (full hue circle), s: 0-255, v: 0-255
constexpr RGB hsvToRgb(uint8_t h, uint8_t s, uint8_t v) {
    if (s == 0) return {v, v, v};

    // Scale hue to 0-1535 (6 sectors * 256)
    uint16_t hue = static_cast<uint16_t>(h) * 6;
    uint8_t sector = static_cast<uint8_t>(hue >> 8);    // 0-5
    uint8_t frac = static_cast<uint8_t>(hue & 0xFF);    // 0-255 within sector

    // p = v * (1 - s), q = v * (1 - s*frac), t = v * (1 - s*(1-frac))
    uint8_t p = static_cast<uint8_t>((static_cast<uint16_t>(v) * (255 - s)) >> 8);
    uint8_t q = static_cast<uint8_t>((static_cast<uint16_t>(v) * (255 - ((static_cast<uint16_t>(s) * frac) >> 8))) >> 8);
    uint8_t t = static_cast<uint8_t>((static_cast<uint16_t>(v) * (255 - ((static_cast<uint16_t>(s) * (255 - frac)) >> 8))) >> 8);

    switch (sector) {
        case 0:  return {v, t, p};
        case 1:  return {q, v, p};
        case 2:  return {p, v, t};
        case 3:  return {p, q, v};
        case 4:  return {t, p, v};
        default: return {v, p, q};
    }
}

// scale8: (val * scale) / 256, with +1 correction so scale8(x, 255) == x
constexpr uint8_t scale8(uint8_t val, uint8_t scale) {
    return static_cast<uint8_t>(((static_cast<uint16_t>(val) * static_cast<uint16_t>(scale)) + 1 + ((static_cast<uint16_t>(val) * static_cast<uint16_t>(scale)) >> 8)) >> 8);
}

// 256-entry sine LUT: sin(2*pi*i/256)*127+128, stored in flash.
// Integer-only sin/cos for effects. Use sin8 for sine, cos8 = sin8(i + 64).
inline constexpr uint8_t sin8_lut[256] = {
    128,131,134,137,140,144,147,150,153,156,159,162,165,168,171,174,
    177,179,182,185,188,191,193,196,199,201,204,206,209,211,213,216,
    218,220,222,224,226,228,230,232,234,235,237,239,240,241,243,244,
    245,246,248,249,250,250,251,252,253,253,254,254,254,255,255,255,
    255,255,255,255,254,254,254,253,253,252,251,250,250,249,248,246,
    245,244,243,241,240,239,237,235,234,232,230,228,226,224,222,220,
    218,216,213,211,209,206,204,201,199,196,193,191,188,185,182,179,
    177,174,171,168,165,162,159,156,153,150,147,144,140,137,134,131,
    128,125,122,119,116,112,109,106,103,100,97,94,91,88,85,82,
    79,77,74,71,68,65,63,60,57,55,52,50,47,45,43,40,
    38,36,34,32,30,28,26,24,22,21,19,17,16,15,13,12,
    11,10,8,7,6,6,5,4,3,3,2,2,2,1,1,1,
    1,1,1,1,2,2,2,3,3,4,5,6,6,7,8,10,
    11,12,13,15,16,17,19,21,22,24,26,28,30,32,34,36,
    38,40,43,45,47,50,52,55,57,60,63,65,68,71,74,77,
    79,82,85,88,91,94,97,100,103,106,109,112,116,119,122,125
};

constexpr uint8_t sin8(uint8_t i) { return sin8_lut[i]; }
constexpr uint8_t cos8(uint8_t i) { return sin8_lut[static_cast<uint8_t>(i + 64)]; }

// Fast octant atan2: returns 0-255 for full circle (y, x) in -32768..32767
constexpr uint8_t atan2_8(int16_t y, int16_t x) {
    uint8_t r = 0;
    if (y < 0) { y = static_cast<int16_t>(-y); r = 0x80; }
    if (x < 0) { x = static_cast<int16_t>(-x); r = static_cast<uint8_t>(r | 0x40); }
    uint8_t offset = (x > y) ? 0 : 32;
    if (x < y) {
        int16_t t = y;
        y = x;
        x = t;
    }
    uint8_t b = (x == 0) ? 0 : static_cast<uint8_t>((static_cast<uint16_t>(y) * 64) / static_cast<uint16_t>(x));
    return static_cast<uint8_t>(r + offset + b);
}

// Octagonal distance approximation (no sqrt)
constexpr uint8_t dist8(int16_t dx, int16_t dy) {
    int16_t ax = dx < 0 ? static_cast<int16_t>(-dx) : dx;
    int16_t ay = dy < 0 ? static_cast<int16_t>(-dy) : dy;
    return static_cast<uint8_t>(ax > ay ? ax + (ay >> 1) : ay + (ax >> 1));
}

} // namespace mm
