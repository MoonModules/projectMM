#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "platform_config.h"

namespace mm {

// Type sizes depend on PSRAM availability (set per-platform in platform_config.h).
// With PSRAM: larger types for big installations (10K+ lights, large coordinates).
// Without PSRAM: smaller nrOfLightsType to minimize LUT memory.
// lengthType stays int16_t everywhere — int8_t can't hold 128 and the savings
// only matter in MappingLUT (not yet implemented).
using nrOfLightsType = std::conditional_t<platform::hasPsram, uint32_t, uint16_t>;
using lengthType = int16_t;

constexpr lengthType defaultGridSize = 128;

// Minimum heap to preserve for stack, HTTP, WiFi, and overhead
constexpr size_t HEAP_RESERVE = 32768;

// Callback for layout coordinate iteration
using CoordCallback = void(*)(void* ctx, nrOfLightsType idx, lengthType x, lengthType y, lengthType z);

} // namespace mm
