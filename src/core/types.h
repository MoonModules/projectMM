#pragma once

#include <cstdint>

namespace mm {

// Desktop uses larger types (PSRAM-like).
// ESP32 without PSRAM: uint16_t / int8_t (selected at compile time).
using nrOfLightsType = uint32_t;
using lengthType = int16_t;

// Default grid size: 128 everywhere except ESP32 without PSRAM (16).
constexpr lengthType defaultGridSize = 128;

// Callback for layout coordinate iteration
using CoordCallback = void(*)(void* ctx, nrOfLightsType idx, lengthType x, lengthType y, lengthType z);

} // namespace mm
