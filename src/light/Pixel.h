#pragma once

#include <cstdint>

namespace mm::light {

constexpr uint8_t scale8(uint8_t val, uint8_t scale) {
    return static_cast<uint8_t>((static_cast<uint16_t>(val) * static_cast<uint16_t>(scale)) >> 8);
}

struct RGB {
    uint8_t r, g, b;

    static constexpr RGB black() { return {0, 0, 0}; }
    static constexpr RGB white() { return {255, 255, 255}; }

    // Integer HSV to RGB (6-sector, no floats)
    // h: 0-255 hue, s: 0-255 saturation, v: 0-255 value
    static constexpr RGB fromHSV(uint8_t h, uint8_t s, uint8_t v) {
        if (s == 0) return {v, v, v};

        uint8_t region = h / 43;
        uint8_t remainder = (h - region * 43) * 6;

        uint8_t p = scale8(v, 255 - s);
        uint8_t q = scale8(v, 255 - scale8(s, remainder));
        uint8_t t = scale8(v, 255 - scale8(s, 255 - remainder));

        switch (region) {
            case 0:  return {v, t, p};
            case 1:  return {q, v, p};
            case 2:  return {p, v, t};
            case 3:  return {p, q, v};
            case 4:  return {t, p, v};
            default: return {v, p, q};
        }
    }
};

static_assert(sizeof(RGB) == 3);

constexpr RGB blend(RGB a, RGB b, uint8_t amount) {
    return {
        static_cast<uint8_t>(a.r + scale8(static_cast<uint8_t>(b.r - a.r), amount)),
        static_cast<uint8_t>(a.g + scale8(static_cast<uint8_t>(b.g - a.g), amount)),
        static_cast<uint8_t>(a.b + scale8(static_cast<uint8_t>(b.b - a.b), amount)),
    };
}

} // namespace mm::light
