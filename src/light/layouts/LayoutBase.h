#pragma once

// The single include a layout needs: LayoutBase plus the maths a coordinate placement reaches for.
// A layout overrides lightCount() and placeLights(), reporting each light's position.

#include "core/module/MoonModule.h"
#include "light/util/light_types.h" // lengthType, nrOfLightsType, Coord3D
#include "core/util/math8.h"        // sin8/cos8/atan2_8, integer trig for circular layouts

#include <cmath>              // sinf/cosf/fmodf, float trig where a layout needs it
#include <cstdint>          // fixed-width ints
#include <limits>          // std::numeric_limits, the lightCount clamp GridLayout uses
#include <numbers>       // std::numbers::pi_v, portable pi for ring layouts

namespace mm {

/// The per-light function a `CoordSink` carries, taking a physical index and a position.
using CoordCallback = void(*)(void* ctx, nrOfLightsType idx, lengthType x, lengthType y, lengthType z);

// The builder/visitor-sink shape, one named method per kind, avoiding a boolean parameter.
/// The sink a layout emits its positions into, with one method per kind of pixel.
struct CoordSink {
    /// The lit-pixel handler, which every layout reaches.
    CoordCallback cb;
    /// The gap handler, or null to let a gap fall back to `cb`.
    CoordCallback blackCb;
    /// The consumer's own state, handed back to whichever handler runs.
    void* ctx;

    /// A normal light at physical index `idx`, position (x,y,z). What every layout calls.
    void pixel(nrOfLightsType idx, lengthType x, lengthType y, lengthType z) const {
        cb(ctx, idx, x, y, z);
    }
    /// A gap light: a physical slot that stays black, for a layout with dark regions.
    void blackPixel(nrOfLightsType idx, lengthType x, lengthType y, lengthType z) const {
        (blackCb ? blackCb : cb)(ctx, idx, x, y, z);
    }
};

/// Base for one layout child of the `Layouts` container, placing lights in space.
///
/// A concrete layout implements `lightCount` and `placeLights` directly.
/// Every layout control changes the light count, so any change rebuilds the pipeline.
class LayoutBase : public MoonModule {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Layout; }
    /// How many physical lights this layout places.
    virtual nrOfLightsType lightCount() const = 0;
    /// Emit every light's position into the sink, in physical index order.
    virtual void placeLights(const CoordSink& sink) const = 0;

    // Advisory only: every layout states it, and nothing in the render path reads it.
    /// The shape this layout lays out: a line, a picture, or a volume.
    virtual Dim dimensions() const { return Dim::D2; }

    // Gates the dense-identity fast path off, since an identity map would light a gap.
    /// Whether this layout holds any physical slots dark.
    virtual bool hasBlackPixels() const { return false; }

    /// Whether a control change rebuilds the pipeline, which every layout control does.
    bool affectsPrepare(const char* /*controlName*/) const override { return true; }
};

} // namespace mm
