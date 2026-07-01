#pragma once

#include <cstdint>

// Value noise: a smooth, deterministic pseudo-random field, the staple "organic motion"
// source for LED effects. inoise8 returns a 0..255 value that varies smoothly across space,
// so neighbouring coordinates give similar values (unlike a raw hash). Sample it across a
// grid for clouds/plasma/fire-like fields; scroll a coordinate (or pass a time offset) to
// animate. 1D, 2D and 3D variants share one hash + a smoothstep interpolation.
//
// Prior art: FastLED's inoise8 (Perlin/value noise; Mark Kriegsman / Ken Perlin's method).
// Same recognisable name + 0..255 contract; the hash + smoothstep + lerp here are ours,
// promoted from NoiseEffect's own implementation so every effect shares one field generator.
//
// Coordinates are 16.0 fixed scaled however the caller likes — the high byte selects the
// noise CELL, the low byte the interpolation position within it. So a larger coordinate step
// per pixel = finer noise (more cells across the grid); a smaller step = broader, smoother.

namespace mm {
namespace noise {

// Integer hash → 0..255. Three coords (z=0 for 1D/2D) feed one well-mixed avalanche.
constexpr uint8_t hash(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t h = x * 1619u + y * 31337u + z * 6271u;
    h = (h >> 13) ^ h;
    h = h * (h * h * 60493u + 19990303u) + 1376312589u;
    return static_cast<uint8_t>((h >> 16) & 0xFF);
}

// Smoothstep 3t²−2t³ on 0..255 — turns the linear cell fraction into an eased one so the
// field has no hard creases at cell boundaries (the difference between value noise and a
// blocky grid).
constexpr uint8_t smoothstep(uint8_t t) {
    uint16_t t2 = static_cast<uint16_t>(t) * t / 255;
    uint16_t t3 = static_cast<uint16_t>(t2) * t / 255;
    return static_cast<uint8_t>((3 * t2 - 2 * t3) & 0xFF);
}

// Linear interpolate a→b by t/255.
constexpr uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t) {
    int16_t delta = static_cast<int16_t>(b) - static_cast<int16_t>(a);
    return static_cast<uint8_t>(static_cast<int16_t>(a) + delta * t / 255);
}

}  // namespace noise

// 1D value noise: x is a 16.0 fixed coordinate (high byte = cell, low byte = position).
constexpr uint8_t inoise8(uint32_t x) {
    const uint32_t ix = x >> 8;
    const uint8_t fx = noise::smoothstep(static_cast<uint8_t>(x & 0xFF));
    return noise::lerp8(noise::hash(ix, 0, 0), noise::hash(ix + 1, 0, 0), fx);
}

// 2D value noise with bilinear interpolation over the 4 cell corners.
constexpr uint8_t inoise8(uint32_t x, uint32_t y) {
    const uint32_t ix = x >> 8, iy = y >> 8;
    const uint8_t fx = noise::smoothstep(static_cast<uint8_t>(x & 0xFF));
    const uint8_t fy = noise::smoothstep(static_cast<uint8_t>(y & 0xFF));
    const uint8_t v00 = noise::hash(ix,     iy,     0);
    const uint8_t v10 = noise::hash(ix + 1, iy,     0);
    const uint8_t v01 = noise::hash(ix,     iy + 1, 0);
    const uint8_t v11 = noise::hash(ix + 1, iy + 1, 0);
    return noise::lerp8(noise::lerp8(v00, v10, fx), noise::lerp8(v01, v11, fx), fy);
}

// 3D value noise with trilinear interpolation over the 8 cube corners.
constexpr uint8_t inoise8(uint32_t x, uint32_t y, uint32_t z) {
    const uint32_t ix = x >> 8, iy = y >> 8, iz = z >> 8;
    const uint8_t fx = noise::smoothstep(static_cast<uint8_t>(x & 0xFF));
    const uint8_t fy = noise::smoothstep(static_cast<uint8_t>(y & 0xFF));
    const uint8_t fz = noise::smoothstep(static_cast<uint8_t>(z & 0xFF));
    const uint8_t v000 = noise::hash(ix,     iy,     iz);
    const uint8_t v100 = noise::hash(ix + 1, iy,     iz);
    const uint8_t v010 = noise::hash(ix,     iy + 1, iz);
    const uint8_t v110 = noise::hash(ix + 1, iy + 1, iz);
    const uint8_t v001 = noise::hash(ix,     iy,     iz + 1);
    const uint8_t v101 = noise::hash(ix + 1, iy,     iz + 1);
    const uint8_t v011 = noise::hash(ix,     iy + 1, iz + 1);
    const uint8_t v111 = noise::hash(ix + 1, iy + 1, iz + 1);
    const uint8_t z0 = noise::lerp8(noise::lerp8(v000, v100, fx), noise::lerp8(v010, v110, fx), fy);
    const uint8_t z1 = noise::lerp8(noise::lerp8(v001, v101, fx), noise::lerp8(v011, v111, fx), fy);
    return noise::lerp8(z0, z1, fz);
}

}  // namespace mm
