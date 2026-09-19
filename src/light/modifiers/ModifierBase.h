#pragma once

// The single include a modifier needs: ModifierBase plus the maths a coordinate fold reaches for.
// A modifier overrides one or more of modifyLogicalSize, modifyLogical and modifyLive.

#include "core/module/MoonModule.h"
#include "light/util/light_types.h" // lengthType, nrOfLightsType, Dim
#include "core/util/math8.h"        // sin8/cos8, integer trig for a rotate modifier

#include <cmath>              // std::sqrt, sin, cos: float trig for circle folds
#include <cstdint>           // fixed-width ints
#include <cstdlib>          // std::abs
#include <algorithm>       // std::max / std::min / std::clamp

namespace mm {

/// A coordinate transform that reshapes how a layer's output maps onto the physical lights.
///
/// Several modifiers on one layer compose, applying in child order.
/// Each reshapes the result of the one below it.
///
/// @moreinfo
///
/// ## The fold contract
///
/// A layer builds its mapping by folding every physical light through each enabled modifier.
/// The whole chain collapses into one table, so the per-frame render stays a single lookup.
/// Three hooks do it, each a no-op by default, so a modifier implements only what it needs.
///
/// ## Fan-out is free
///
/// The build walks physical lights, so several folding onto one logical cell is the fan-out.
/// Each physical light contributes at most one destination, so the mapping cannot overflow.
///
/// ## Affine modifiers
///
/// Most are not affine: a mask is a predicate and a tile is modulo.
/// Rotation is the exception, written as an integer rotation matrix.
///
/// ## Prior art
///
/// The textbook image-warping pattern: bake a transform into a spatial table, built backward.
class ModifierBase : public MoonModule {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Modifier; }

    /// Whether a control change rebuilds the mapping, which every modifier control does.
    bool affectsPrepare(const char* /*controlName*/) const override { return true; }

    // Advisory only, unlike an effect's dimensions: the render path never reads this.
    /// Which axes this modifier can transform.
    virtual Dim dimensions() const { return Dim::D3; }

    // The fold interface, whose composition contract is in the class comment.

    // A modifier needing the box in its per-light fold stashes it here, reading its own stage.
    /// Reshape the running logical box, once per rebuild in child order.
    virtual void modifyLogicalSize(Coord3D& /*size*/) {}

    // A bool rather than a sentinel coordinate, which a later modulo could alias back in range.
    /// Fold one coordinate into this stage's logical space, returning false to reject it.
    virtual bool modifyLogical(Coord3D& /*pos*/) const { return true; }

    // Run only when some enabled modifier overrides it, so a static chain pays nothing.
    /// Remap a coordinate per frame, without rebuilding the mapping.
    virtual void modifyLive(Coord3D& /*pos*/, const Coord3D& /*logical*/) const {}

    /// Whether this modifier does per-frame work, which gates the live pass.
    virtual bool hasModifyLive() const { return false; }

    // Polled once per frame across every enabled modifier, so several coalesce into one rebuild.
    /// Whether this modifier's mapping changed and wants a rebuild, clearing the flag.
    virtual bool consumeNeedsRebuild() { return false; }
};

} // namespace mm
