#pragma once

#include <cstdint>

namespace mm::light {

namespace detail {

// Permutation table (Ken Perlin's original, doubled to avoid wrapping)
inline constexpr uint8_t perm[512] = {
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,
    140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
    247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,
    57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
    74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
    60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
    65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
    200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
    52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,
    207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
    119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
    129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
    218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,
    81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
    184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,
    222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180,
    // repeat
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,
    140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
    247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,
    57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
    74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
    60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
    65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
    200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
    52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,
    207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
    119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
    129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
    218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,
    81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
    184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,
    222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180,
};

inline constexpr int16_t lerp16(int16_t a, int16_t b, uint8_t t) {
    return a + ((static_cast<int32_t>(b - a) * t) >> 8);
}

inline constexpr int16_t grad8(uint8_t hash, int16_t x, int16_t y) {
    uint8_t h = hash & 3;
    int16_t u = (h & 2) ? -x : x;
    int16_t v = (h & 1) ? -y : y;
    return u + v;
}

inline constexpr int16_t grad8_3d(uint8_t hash, int16_t x, int16_t y, int16_t z) {
    uint8_t h = hash & 15;
    int16_t u = h < 8 ? x : y;
    int16_t v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

} // namespace detail

// 2D noise: returns 0-255
inline uint8_t noise8(uint16_t x, uint16_t y) {
    uint8_t xi = static_cast<uint8_t>(x >> 8);
    uint8_t yi = static_cast<uint8_t>(y >> 8);
    uint8_t xf = static_cast<uint8_t>(x & 0xFF);
    uint8_t yf = static_cast<uint8_t>(y & 0xFF);

    uint8_t aa = detail::perm[detail::perm[xi]     + yi];
    uint8_t ab = detail::perm[detail::perm[xi]     + yi + 1];
    uint8_t ba = detail::perm[detail::perm[xi + 1] + yi];
    uint8_t bb = detail::perm[detail::perm[xi + 1] + yi + 1];

    int16_t g00 = detail::grad8(aa, xf, yf);
    int16_t g10 = detail::grad8(ba, xf - 256, yf);
    int16_t g01 = detail::grad8(ab, xf, yf - 256);
    int16_t g11 = detail::grad8(bb, xf - 256, yf - 256);

    int16_t x0 = detail::lerp16(g00, g10, xf);
    int16_t x1 = detail::lerp16(g01, g11, xf);
    int16_t val = detail::lerp16(x0, x1, yf);

    return static_cast<uint8_t>((val + 128) & 0xFF);
}

// 3D noise: returns 0-255
inline uint8_t noise8(uint16_t x, uint16_t y, uint16_t z) {
    uint8_t xi = static_cast<uint8_t>(x >> 8);
    uint8_t yi = static_cast<uint8_t>(y >> 8);
    uint8_t zi = static_cast<uint8_t>(z >> 8);
    uint8_t xf = static_cast<uint8_t>(x & 0xFF);
    uint8_t yf = static_cast<uint8_t>(y & 0xFF);
    uint8_t zf = static_cast<uint8_t>(z & 0xFF);

    uint8_t aaa = detail::perm[detail::perm[detail::perm[xi]     + yi]     + zi];
    uint8_t aab = detail::perm[detail::perm[detail::perm[xi]     + yi]     + zi + 1];
    uint8_t aba = detail::perm[detail::perm[detail::perm[xi]     + yi + 1] + zi];
    uint8_t abb = detail::perm[detail::perm[detail::perm[xi]     + yi + 1] + zi + 1];
    uint8_t baa = detail::perm[detail::perm[detail::perm[xi + 1] + yi]     + zi];
    uint8_t bab = detail::perm[detail::perm[detail::perm[xi + 1] + yi]     + zi + 1];
    uint8_t bba = detail::perm[detail::perm[detail::perm[xi + 1] + yi + 1] + zi];
    uint8_t bbb = detail::perm[detail::perm[detail::perm[xi + 1] + yi + 1] + zi + 1];

    int16_t g000 = detail::grad8_3d(aaa, xf, yf, zf);
    int16_t g100 = detail::grad8_3d(baa, xf - 256, yf, zf);
    int16_t g010 = detail::grad8_3d(aba, xf, yf - 256, zf);
    int16_t g110 = detail::grad8_3d(bba, xf - 256, yf - 256, zf);
    int16_t g001 = detail::grad8_3d(aab, xf, yf, zf - 256);
    int16_t g101 = detail::grad8_3d(bab, xf - 256, yf, zf - 256);
    int16_t g011 = detail::grad8_3d(abb, xf, yf - 256, zf - 256);
    int16_t g111 = detail::grad8_3d(bbb, xf - 256, yf - 256, zf - 256);

    int16_t x00 = detail::lerp16(g000, g100, xf);
    int16_t x10 = detail::lerp16(g010, g110, xf);
    int16_t x01 = detail::lerp16(g001, g101, xf);
    int16_t x11 = detail::lerp16(g011, g111, xf);

    int16_t y0 = detail::lerp16(x00, x10, yf);
    int16_t y1 = detail::lerp16(x01, x11, yf);

    int16_t val = detail::lerp16(y0, y1, zf);
    return static_cast<uint8_t>((val + 128) & 0xFF);
}

// 16-bit variants for higher precision
inline uint16_t noise16(uint32_t x, uint32_t y) {
    return static_cast<uint16_t>(noise8(static_cast<uint16_t>(x >> 8),
                                        static_cast<uint16_t>(y >> 8))) << 8;
}

inline uint16_t noise16(uint32_t x, uint32_t y, uint32_t z) {
    return static_cast<uint16_t>(noise8(static_cast<uint16_t>(x >> 8),
                                        static_cast<uint16_t>(y >> 8),
                                        static_cast<uint16_t>(z >> 8))) << 8;
}

} // namespace mm::light
