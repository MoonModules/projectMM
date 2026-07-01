#pragma once

#include <cstdint>
#include <type_traits>
#include "platform_config.h"

// The foundational coordinate / count / dimension types of the light domain,
// shared by ~20 files (every effect, modifier, layout, layer, driver, buffer)
// with no single owner — so they live in one shared header rather than being
// scattered into an arbitrary one. Symbols that DO have a single owner live
// there instead (defaultGridSize in GridLayout.h, CoordCallback in Layouts.h).
// This header includes only the platform layer, never core — the boundary
// stays one-directional. Core names none of these types: ModuleFactory captures
// a module's dimensionality via a return-type-agnostic `dimensions()` probe, so
// even `Dim` stays here.

namespace mm {

// A grid coordinate. int16_t everywhere — int8_t can't hold a 128-edge grid,
// and a smaller type would only save in MappingLUT, which doesn't yet do that
// size optimisation (it stores nrOfLightsType indices, not coordinates).
using lengthType = int16_t;

// A 3D grid position / size. Modifiers fold one of these in place when building
// the Layer's mapping (each modifier is a coordinate transform — Multiply does
// `pos = pos % size`, a mirror folds an axis), so the per-axis `%` `/` `-` `+`
// operators below let the fold read like the geometry it expresses rather than
// three separate index lines. A struct, not a class: it's plain data on the cold
// build path (the hot render path stays on flat nrOfLightsType indices, never
// these). Operators are per-component (Hadamard), the convention for a grid/size
// vector — `a % b` is `{a.x%b.x, a.y%b.y, a.z%b.z}`.
struct Coord3D {
    lengthType x = 0, y = 0, z = 0;

    Coord3D operator+(const Coord3D& o) const { return {static_cast<lengthType>(x + o.x), static_cast<lengthType>(y + o.y), static_cast<lengthType>(z + o.z)}; }
    Coord3D operator-(const Coord3D& o) const { return {static_cast<lengthType>(x - o.x), static_cast<lengthType>(y - o.y), static_cast<lengthType>(z - o.z)}; }
    Coord3D operator*(const Coord3D& o) const { return {static_cast<lengthType>(x * o.x), static_cast<lengthType>(y * o.y), static_cast<lengthType>(z * o.z)}; }
    // Per-axis %/÷ guard against a zero extent (a degenerate axis stays put / 0)
    // so a modifier can fold without pre-checking every axis for size 1 or 0.
    Coord3D operator%(const Coord3D& o) const { return {modAxis(x, o.x), modAxis(y, o.y), modAxis(z, o.z)}; }
    Coord3D operator/(const Coord3D& o) const { return {divAxis(x, o.x), divAxis(y, o.y), divAxis(z, o.z)}; }
    bool operator==(const Coord3D& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Coord3D& o) const { return !(*this == o); }

private:
    static lengthType modAxis(lengthType a, lengthType m) { return m > 0 ? static_cast<lengthType>(a % m) : a; }
    static lengthType divAxis(lengthType a, lengthType d) { return d > 0 ? static_cast<lengthType>(a / d) : a; }
};

// Count of lights, and the index type MappingLUT stores. uint32_t with PSRAM
// (10K+ lights, large installations), uint16_t without — the narrower index
// keeps MappingLUT's CSR arrays half the size on no-PSRAM boards. Selected at
// compile time from platform_config.h's hasPsram flag.
using nrOfLightsType = std::conditional_t<platform::hasPsram, uint32_t, uint16_t>;

// Dimensional support. Effects use this to declare which axes they iterate so the
// Layer can extrude lower-dimensional output across unused axes (D1 column → fill x/z,
// D2 slice → fill z, D3 → no extrusion). Modifiers use it to advertise which axes
// they can transform. The UI derives the 📏/🟦/🧊 chip from it (captured generically
// by core's ModuleFactory, which detects a `dimensions()` method without naming
// this type — so the enum stays in the light domain). EffectBase and ModifierBase
// both refer to it without one including the other.
enum class Dim : uint8_t {
    D1 = 1,
    D2 = 2,
    D3 = 3,
};

} // namespace mm
