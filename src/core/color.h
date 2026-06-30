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

// scale8: (val * scale) / 256, with +1 correction so scale8(x, 255) == x. The fundamental
// channel-scale op (brightness, blend), kept here with the colour type it scales. Integer
// trig (sin8/cos8/atan2_8/dist8) and the rest of the 8-bit math library live in math8.h.
constexpr uint8_t scale8(uint8_t val, uint8_t scale) {
    return static_cast<uint8_t>(((static_cast<uint16_t>(val) * static_cast<uint16_t>(scale)) + 1 + ((static_cast<uint16_t>(val) * static_cast<uint16_t>(scale)) >> 8)) >> 8);
}

} // namespace mm
