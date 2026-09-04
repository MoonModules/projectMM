#pragma once

#include "core/Control.h"         // ControlList: the controls an effect surfaces for the table
#include "core/ScratchBuffer.h"   // ScratchBuffer<T>: self-sizing, owner-tied scratch memory
#include "core/math16.h"          // atan16, dist16, angle16
#include "light/light_types.h"    // lengthType
#include "platform/platform.h"    // freeHeap, HEAP_RESERVE: the memory gate

#include <cstdint>

// PolarLut: the angle and radius of every pixel, computed once and read from a table.
//
// A radial effect addresses the grid by ANGLE and RADIUS rather than x and y, which is what makes
// its motion turn around the center instead of sliding across it. Computed per pixel per frame,
// that address is `atan16` plus `dist16`, and on an ESP32-S3 it measures 1.9 microseconds per
// pixel: 39% of a PolarNoise frame at 64x64, before the field it addresses is sampled at all. The
// address does not change between frames, only the field sampled through it does, so the whole cost
// is recomputation of a constant.
//
// This is that constant, held per layer: two tables the size of the grid, rebuilt when the geometry
// changes and read as two array lookups per pixel afterwards. Effects keep their look and get their
// frame time back, and it compounds, because every radial effect pays the same toll today.
//
// Two widths, because the tradeoff is real. The 8-bit tables cost 2 bytes per pixel and are the
// default: 256 angle steps and 256 radius steps are what an effect sampling a noise field through
// them can actually resolve. The 16-bit tables cost 4 bytes per pixel and are opt-in, for an effect
// whose look breaks up at 8 bits: a slow rotation shows angle steps as visible facets, and a
// palette indexed straight from the radius bands. On a 128x128 wall that is 32 KB against 64 KB.
//
// The tables are a layer's, not an effect's: every effect on that layer shares one address for the
// same grid. An effect declares a PolarLut member, calls prepare() with the grid it is about to
// draw, and reads. prepare() rebuilds only when the geometry or the width actually changed, so
// calling it every frame is free.
//
// On a VOLUMETRIC fixture "angle and radius" has no single meaning, so the projection is a control
// rather than a decision. Which one is right is a property of the fixture, not of the library:
//
//   cylindrical - angle and radius on the xy plane, depth carried separately. A tube, a curtain or
//                 a stack of panels wants this, and it reduces to exactly the 2D behavior when
//                 depth is 1, so no existing fixture changes. The default for that reason.
//   spherical   - angle around, angle up, radius from the center. A sphere or a ball of light
//                 inside a cube wants this; it costs one more table for the second angle.
//   radial      - distance from the center only, no angle. Shells rather than curtains, and the
//                 cheapest of the three.
//
// The choice is made once when the table is built, so it costs nothing per sample.
//
// On a device without PSRAM the tables are a real fraction of the heap, so prepare() checks free
// memory against the reserve and declines rather than crowding it: a 32x32 grid asks 2 KB, a 64x64
// grid 8 KB, and a 128x128 wall 32 KB. The caller falls back to computing the address per pixel and
// the picture is unchanged, which is why the fallback is a supported path rather than an error
// case, and why an effect exposes it as a control.

namespace mm {

/// The polar address of every pixel on a grid, as tables.
///
/// Precision is chosen at prepare() time rather than by type, so an effect can offer it as a
/// control and switch live, which is what the live-reconfiguration rule requires.
class PolarLut {
public:
    /// How a volumetric fixture's coordinates become an angle and a radius. Ignored at depth 1,
    /// where all three reduce to the same plane.
    enum class Mapping : uint8_t {
        Cylindrical = 0,   ///< angle and radius on xy, depth separate: the 2D behavior, extended
        Spherical   = 1,   ///< angle around and angle up, radius from the center
        Radial      = 2,   ///< distance from the center only
    };

    /// Owner is the module the tables belong to: it accounts their bytes and frees them when the
    /// module is disabled, the same contract every ScratchBuffer has.
    explicit PolarLut(MoonModule& owner)
        : angle8_(owner), radius8_(owner), pitch8_(owner),
          angle16_(owner), pitch16_(owner), radius16_(owner) {}

    /// Build the tables for a `w` x `h` grid, if they are not already built for exactly that.
    /// `wide` selects the 16-bit tables. The center is the grid's middle in whole pixels, matching
    /// what the radial effects compute by hand today.
    ///
    /// Returns false when the tables are not available, and the caller then computes the address per
    /// pixel: an effect must still render on a device too tight for them, just more slowly. Two
    /// things make it decline, and the first matters more on a device without PSRAM. It REFUSES the
    /// tables when free heap minus the reserve that protects stacks, WiFi and HTTP cannot hold them,
    /// rather than taking the last of the heap and starving something else later (the rule
    /// MappingLUT established). And if the allocation fails anyway it unwinds to nothing rather than
    /// holding a half-built table. Degrade visibly, never crash.
    /// The controls an effect surfaces to let a user address a volumetric fixture, and the state
    /// behind them. Bound by `addControls` and read by `prepareFor`, so an effect adopting the table
    /// writes two calls rather than repeating a member, an option table, a binding and a five-line
    /// prepare: three effects had all four copied before this existed.
    struct Controls {
        bool    use = true;      ///< read the address from a table rather than computing it
        bool    wide = false;    ///< hold it at full 16-bit precision, at twice the memory
        uint8_t mapping = 0;     ///< which projection, indexing kMappingOptions
    };

    static constexpr const char* kMappingOptions[] = {"cylindrical", "spherical", "radial"};

    /// Surface the three controls on `list`.
    ///
    /// Deliberately asks nothing about the fixture: defineControls() runs before an effect is
    /// attached to a layer (and on the throwaway instances the /api/types probe builds), so
    /// reaching for width or depth here dereferences a null layer. An earlier version hid the
    /// mapping on a flat fixture and segfaulted the framerate sweep for exactly that reason. The
    /// mapping is simply always shown; on a fixture with no depth all three agree, so the control
    /// is inert rather than wrong.
    static void addControls(ControlList& list, Controls& c) {
        list.addControl("polarTable", c.use);
        list.addControl("polarTable16", c.wide);
        list.addSelect("mapping", c.mapping, kMappingOptions, 3);
    }

    /// Build (or release) the table for a fixture of this size, per the controls. The one call an
    /// effect makes from prepare().
    bool prepareFor(const Controls& c, lengthType w, lengthType h, lengthType d) {
        if (!c.use) { release(); return false; }
        return prepare(static_cast<uint16_t>(w), static_cast<uint16_t>(h), static_cast<uint16_t>(d),
                       c.wide, static_cast<Mapping>(c.mapping > 2 ? 0 : c.mapping));
    }

    /// The 2D form. A separate overload rather than a defaulted depth, because `prepare(w, h, true)`
    /// meaning "16-bit" and `prepare(w, h, 1)` meaning "one deep" are one careless argument apart,
    /// and a bool that silently becomes a depth is the kind of mistake that compiles and renders
    /// wrong. The arity says which one the caller means.
    bool prepare(uint16_t w, uint16_t h, bool wide = false) {
        return prepare(w, h, 1, wide, Mapping::Cylindrical);
    }

    /// The volumetric form: `d` is the fixture's depth and `mapping` how its coordinates become an
    /// angle and a radius. At `d` = 1 all three mappings agree and this is the 2D form.
    bool prepare(uint16_t w, uint16_t h, uint16_t d, bool wide,
                 Mapping mapping = Mapping::Cylindrical) {
        if (w == w_ && h == h_ && d == d_ && wide == wide_ && mapping == mapping_ && ready_) return true;
        w_ = w; h_ = h; d_ = d; wide_ = wide; mapping_ = mapping; ready_ = false;
        const std::size_t n = static_cast<std::size_t>(w) * h * (d ? d : 1);
        if (n == 0) { release(); return false; }      // an empty grid holds nothing, not the old table

        // Spherical is the one projection that needs a second angle, so it is the one that costs a
        // third table. The others leave it unallocated rather than filled with zeros.
        const bool needPitch = mapping == Mapping::Spherical && d > 1;

        // Free the width we are not using, so switching precision live does not hold both, and so
        // the budget below is measured against a heap that already has the old tables back.
        if (wide) { angle8_.resize(0); radius8_.resize(0); pitch8_.resize(0); }
        else      { angle16_.resize(0); radius16_.resize(0); pitch16_.resize(0); }
        if (!needPitch) { pitch8_.resize(0); pitch16_.resize(0); }

        // The memory gate. freeHeap() is 0 on desktop, which means unlimited, so the gate is an
        // ESP32 rule only. The tables are worth having, not worth the last of the heap.
        const std::size_t want = n * (wide ? 4u : 2u) + (needPitch ? n * (wide ? 2u : 1u) : 0u);
        const std::size_t freeHeap = platform::freeHeap();
        if (freeHeap != 0) {
            const std::size_t budget = freeHeap > platform::HEAP_RESERVE ? freeHeap - platform::HEAP_RESERVE : 0;
            if (budget < want) { release(); return false; }
        }

        if (wide) {
            if (!angle16_.resize(n) || !radius16_.resize(n)) { release(); return false; }
            if (needPitch && !pitch16_.resize(n)) { release(); return false; }
        } else {
            if (!angle8_.resize(n) || !radius8_.resize(n)) { release(); return false; }
            if (needPitch && !pitch8_.resize(n)) { release(); return false; }
        }

        // The longest radius on the fixture: the corner furthest from the center. Radii scale
        // against it so the outer edge reaches full scale whatever the aspect ratio, which is what
        // makes a radial gradient fill a wide panel instead of banding in a circle inside it. Under
        // cylindrical the depth axis is carried separately, so it does not enter the radius and a
        // deep fixture measures the same as the panel it is made of.
        const int32_t cx = w / 2, cy = h / 2, cz = d / 2;
        const int32_t fx = (cx > w - 1 - cx ? cx : w - 1 - cx);
        const int32_t fy = (cy > h - 1 - cy ? cy : h - 1 - cy);
        const int32_t fz = (cz > d - 1 - cz ? cz : d - 1 - cz);
        maxRadius_ = mapping == Mapping::Cylindrical ? dist16(fx, fy)
                                                     : dist16(dist16(fx, fy), fz);
        if (maxRadius_ == 0) maxRadius_ = 1;               // a 1x1 grid: avoid a divide by zero

        for (uint16_t z = 0; z < (d ? d : 1); z++)
        for (uint16_t y = 0; y < h; y++) {
            for (uint16_t x = 0; x < w; x++) {
                const int32_t dx = static_cast<int32_t>(x) - cx;
                const int32_t dy = static_cast<int32_t>(y) - cy;
                const int32_t dz = static_cast<int32_t>(z) - cz;

                // The three projections differ only here: what the angle means and what the radius
                // measures. Everything below is common, which is why adding one costs a case.
                angle16 a = 0;
                uint32_t r = 0;
                angle16 pitch = 0;
                switch (mapping) {
                    case Mapping::Radial:
                        // No angle at all: distance from the center, so the field reads as shells.
                        r = dist16(dist16(dx, dy), dz);
                        break;
                    case Mapping::Spherical:
                        // Around and up: the second angle is the elevation above the xy plane, so a
                        // sphere's surface maps onto the field evenly rather than pinching.
                        a = atan16(dy, dx);
                        r = dist16(dist16(dx, dy), dz);
                        pitch = atan16(dz, static_cast<int32_t>(dist16(dx, dy)));
                        break;
                    case Mapping::Cylindrical:
                    default:
                        // The 2D address, with depth left to the caller: identical to a panel's at
                        // every z, which is what makes this the default.
                        a = atan16(dy, dx);
                        r = dist16(dx, dy);
                        break;
                }
                // Scale the radius to full range against the furthest corner, so `radius` is a
                // position from center (0) to edge (full scale) rather than a pixel count. Both
                // steps ROUND rather than truncate: a caller scaling back to pixels truncates twice
                // otherwise and lands a whole pixel short at every radius, which shifts the entire
                // field inward by one and is plainly visible as a displaced center.
                const uint32_t rs = r >= maxRadius_ ? 65535u : (r * 65535u + maxRadius_ / 2) / maxRadius_;
                const std::size_t i = (static_cast<std::size_t>(z) * h + y) * w + x;
                if (wide) {
                    angle16_[i]  = a;
                    radius16_[i] = static_cast<uint16_t>(rs);
                    if (needPitch) pitch16_[i] = pitch;
                } else {
                    angle8_[i]  = static_cast<uint8_t>(a >> 8);
                    radius8_[i] = static_cast<uint8_t>((rs + 128) >> 8 > 255 ? 255 : (rs + 128) >> 8);
                    if (needPitch) pitch8_[i] = static_cast<uint8_t>(pitch >> 8);
                }
            }
        }
        ready_ = true;
        return true;
    }

    /// Free the tables, for a caller that has switched the address back to per-pixel computation.
    /// The next prepare() rebuilds them.
    void release() {
        if (!ready_ && bytes() == 0) return;
        angle8_.resize(0); radius8_.resize(0); pitch8_.resize(0);
        angle16_.resize(0); radius16_.resize(0); pitch16_.resize(0);
        w_ = h_ = d_ = 0;
        ready_ = false;
    }

    /// True once the tables hold a grid; false if allocation failed and the caller must compute.
    bool ready() const { return ready_; }
    /// True when the 16-bit tables are the ones built.
    bool wide() const { return wide_; }
    /// The distance from center to the furthest corner, in pixels: what `radius` is scaled against.
    uint32_t maxRadius() const { return maxRadius_; }
    /// What the tables currently cost.
    std::size_t bytes() const {
        return angle8_.bytes() + radius8_.bytes() + pitch8_.bytes()
             + angle16_.bytes() + radius16_.bytes() + pitch16_.bytes();
    }
    /// Which projection the built tables use.
    Mapping mapping() const { return mapping_; }

    /// The elevation of pixel `i` above the xy plane, under the spherical projection. Zero under the
    /// other two, which have no second angle: a caller can read it unconditionally.
    angle16 pitch(std::size_t i) const {
        if (!ready_) return 0;
        if (wide_) return pitch16_.count() ? pitch16_.data()[i] : 0;
        if (!pitch8_.count()) return 0;
        const uint32_t v = pitch8_.data()[i];
        return static_cast<angle16>((v << 8) | v);
    }

    /// The angle of pixel `i` (row-major, y * width + x), as a full angle16 whichever width is
    /// built: the 8-bit table widens, so a caller reads one type and the precision is the table's.
    /// An angle wraps, so the low byte is filled from the high one and a wrapped value stays
    /// continuous across the seam.
    angle16 angle(std::size_t i) const {
        if (!ready_) return 0;
        if (wide_) return angle16_.data()[i];
        const uint32_t v = angle8_.data()[i];
        return static_cast<angle16>((v << 8) | v);
    }

    /// The radius of pixel `i`, 0 at the center to 65535 at the furthest corner.
    uint16_t radius(std::size_t i) const {
        if (!ready_) return 0;
        if (wide_) return radius16_.data()[i];
        // Widen so the top of the 8-bit range reaches the top of the 16-bit one: 255 must read as
        // full scale, not as 65280, or the outer edge never arrives.
        const uint32_t v = radius8_.data()[i];
        return static_cast<uint16_t>((v << 8) | v);
    }

    /// The radius of pixel `i` in PIXELS from the center, which is the form a field scaled in grid
    /// units wants. Rounded, so it round-trips the stored value rather than landing short.
    uint32_t radiusPixels(std::size_t i) const {
        if (!ready_) return 0;
        return (static_cast<uint32_t>(radius(i)) * maxRadius_ + 32768u) >> 16;
    }

    /// The angle at (x, y). The index form above is what a pixel loop should use; this is for a
    /// caller that has coordinates rather than a running index.
    angle16 angleAt(uint16_t x, uint16_t y, uint16_t z = 0) const { return angle(index(x, y, z)); }
    /// The radius at (x, y, z).
    uint16_t radiusAt(uint16_t x, uint16_t y, uint16_t z = 0) const { return radius(index(x, y, z)); }
    /// The elevation at (x, y, z), under the spherical projection.
    angle16 pitchAt(uint16_t x, uint16_t y, uint16_t z = 0) const { return pitch(index(x, y, z)); }
    /// The running index of (x, y, z), the same order the buffer uses.
    std::size_t index(uint16_t x, uint16_t y, uint16_t z = 0) const {
        return (static_cast<std::size_t>(z) * h_ + y) * w_ + x;
    }

private:
    ScratchBuffer<uint8_t>  angle8_, radius8_, pitch8_;
    ScratchBuffer<angle16>  angle16_, pitch16_;
    ScratchBuffer<uint16_t> radius16_;
    uint16_t w_ = 0, h_ = 0, d_ = 0;
    Mapping  mapping_ = Mapping::Cylindrical;
    uint32_t maxRadius_ = 1;
    bool     wide_ = false;
    bool     ready_ = false;
};

}  // namespace mm
