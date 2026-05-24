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

// Dimensional support. Effects use this to declare which axes they iterate so the
// Layer can extrude lower-dimensional output across unused axes (D1 row → fill y/z,
// D2 slice → fill z, D3 → no extrusion). Modifiers use it to advertise which axes
// they can transform. The UI derives the 📏/🟦/🧊 chip from it. Domain-neutral on
// purpose so EffectBase and ModifierBase can both refer to it without one including
// the other.
enum class Dim : uint8_t {
    D1 = 1,
    D2 = 2,
    D3 = 3,
};

} // namespace mm
