#pragma once

#include "core/ScratchBuffer.h"   // the grids an effect owns
#include "light/light_types.h"

#include <cstring>

namespace mm {

// A stable-fluid velocity field: the medium itself, simulated rather than sampled.
//
// Every flow so far has been a FUNCTION of position and time: noise, curl, a wind. This is the
// other kind. The velocity here is state that evolves from its own past, so pushing the medium in
// one place changes where everything downstream goes, and a vortex forms because the math says it
// must rather than because a rule drew one. That is the difference a viewer sees: a curl field is
// beautiful and unchanging in character, while a fluid REACTS.
//
// The algorithm is Stam's (Jos Stam, "Stable Fluids", SIGGRAPH 1999), which is the standard choice
// for exactly one reason: it cannot blow up. An explicit solver has a timestep small enough to stay
// stable, and a frame that runs long breaks it; Stam's is unconditionally stable, so a device that
// stalls for a second resumes with a plausible field instead of a screenful of infinities. On a
// fixture that must never look broken, that property is worth more than accuracy.
//
// Four steps a frame, and the order is the algorithm:
//
//   1. `diffuse`   viscosity: each cell relaxes toward its neighbors' average.
//   2. `project`   make it divergence-free: the step that turns a set of arrows into a FLOW.
//   3. `advect`    the velocity carries itself, which is what makes a vortex persist and travel.
//   4. `project`   again, because advection reintroduces divergence.
//
// Then the caller advects its own dye (the light) along the finished field with `draw::advect16`.
//
// **Q16.16 throughout, not float.** The render path is integer by contract, and a fluid is the
// hardest case for that: `project` solves a linear system by relaxation, so an error that a float
// would absorb accumulates over iterations. 16 fraction bits is what makes the pressure solve
// converge at all; 8 would quantize the gradient to nothing on a slow flow.
//
// Sized for panels. The cost is per cell per iteration and there are several passes, so this is a
// desktop and P4 effect; an S3 runs it on a small grid or not at all.
class Fluid {
public:
    explicit Fluid(MoonModule& owner)
        : vx_(owner), vy_(owner), vx0_(owner), vy0_(owner), p_(owner), div_(owner) {}

    /// Q16.16: the fixed-point format the whole solver works in.
    static constexpr int32_t kOne = 1 << 16;

    /// Size (or free) the grids. Returns whether a field is available, which is what an effect
    /// reports: a device too small says so rather than rendering nothing in silence.
    ///
    /// Depth is a stack of INDEPENDENT slices, each its own 2D medium: the solve is per slice and
    /// nothing is carried between them. A panel is depth 1 and pays nothing for the stack. A true
    /// volumetric solve (pressure and advection across z) is a different solver, not a flag.
    bool resize(lengthType w, lengthType h, lengthType d = 1) {
        if (w <= 2 || h <= 2 || d < 1) { release(); return false; }   // a grid with no interior
        const size_t n = static_cast<size_t>(w) * h * d;
        // Already this shape AND still allocated: MoonModule::release() frees every registered
        // buffer behind this object's back (a disabled module, or a disabled ancestor), and the
        // shape alone would then report a grid that is no longer there.
        if (w == w_ && h == h_ && d == d_ && vx_) return true;
        // The velocity is per cell, but the four working grids are per SLICE: the solve walks one
        // slice at a time and never addresses another's, so sizing them to the volume allocated
        // 19 unused copies on a 20-cube (121 KB for nothing).
        const size_t slice = static_cast<size_t>(w) * h;
        const bool ok = vx_.resize(n) && vy_.resize(n) && vx0_.resize(slice)
                     && vy0_.resize(slice) && p_.resize(slice) && div_.resize(slice);
        if (!ok) { release(); return false; }
        w_ = w; h_ = h; d_ = d; cells_ = n;
        reset();
        return true;
    }

    void release() {
        vx_.resize(0); vy_.resize(0); vx0_.resize(0); vy0_.resize(0); p_.resize(0); div_.resize(0);
        w_ = h_ = d_ = 0; cells_ = 0;
    }

    /// Re-seed to rest: every velocity zero. The resync point, for a fixture that has been
    /// reconfigured under a running simulation.
    void reset() {
        if (!valid()) return;
        std::memset(vx_.data(), 0, vx_.bytes());
        std::memset(vy_.data(), 0, vy_.bytes());
    }

    /// Ready to step. Read from the buffers, not from a cached shape: the owner's release() can
    /// free them between two frames, and that is exactly the frame a stale flag crashes on.
    bool valid() const { return cells_ > 0 && vx_ && vy_ && vx0_ && vy0_ && p_ && div_; }
    lengthType width() const { return w_; }
    lengthType height() const { return h_; }
    lengthType depth() const { return d_; }
    /// Cells per slice: a slice's fields start at `z * plane()` in velocityX()/velocityY().
    size_t plane() const { return static_cast<size_t>(w_) * h_; }
    const int32_t* velocityX() const { return vx_.data(); }
    const int32_t* velocityY() const { return vy_.data(); }

    /// Push the medium at one cell. The source term: an emitter, a control, a beat.
    void addVelocity(lengthType x, lengthType y, int32_t dvx, int32_t dvy, lengthType z = 0) {
        if (!valid() || x < 0 || y < 0 || z < 0 || x >= w_ || y >= h_ || z >= d_) return;
        const size_t i = static_cast<size_t>(z) * plane() + idx(x, y);
        vx_[i] += dvx;
        vy_[i] += dvy;
    }

    /// One frame of the simulation. `viscosity` and `dt` are Q16.16; `iterations` is the pressure
    /// solve's effort, and the honest cost knob: 5 is the usual default, 1 is visibly springy.
    void step(int32_t viscosity, int32_t dt, uint8_t iterations) {
        if (!valid()) return;
        const uint8_t iters = iterationsSanitized(iterations);
        const size_t n = plane();
        for (lengthType z = 0; z < d_; z++) {
            const size_t at = static_cast<size_t>(z) * n;
            int32_t* vx = vx_.data() + at;  int32_t* vy = vy_.data() + at;
            int32_t* vx0 = vx0_.data(); int32_t* vy0 = vy0_.data();   // per-slice scratch
            if (viscosity > 0) {
                diffuse(vx, vx0, viscosity, dt, iters, 1);
                diffuse(vy, vy0, viscosity, dt, iters, 2);
            }
            project(vx, vy, iters);
            std::memcpy(vx0, vx, n * sizeof(int32_t));
            std::memcpy(vy0, vy, n * sizeof(int32_t));
            advectSelf(vx, vy, vx0, vy0, dt);
            project(vx, vy, iters);
        }
    }

private:
    static uint8_t iterationsSanitized(uint8_t iterations) { return iterations < 1 ? 1 : iterations; }
    size_t idx(lengthType x, lengthType y) const { return static_cast<size_t>(y) * w_ + x; }

    /// Walls: the boundary mirrors the interior, with the normal component negated, so the medium
    /// slides along an edge rather than through it. `b` says which component (1 = x, 2 = y, 0 = a
    /// scalar like pressure).
    void setBoundary(int32_t* f, int b) {
        for (lengthType x = 1; x < w_ - 1; x++) {
            f[idx(x, 0)]      = (b == 2) ? -f[idx(x, 1)]      : f[idx(x, 1)];
            f[idx(x, h_ - 1)] = (b == 2) ? -f[idx(x, h_ - 2)] : f[idx(x, h_ - 2)];
        }
        for (lengthType y = 1; y < h_ - 1; y++) {
            f[idx(0, y)]      = (b == 1) ? -f[idx(1, y)]      : f[idx(1, y)];
            f[idx(w_ - 1, y)] = (b == 1) ? -f[idx(w_ - 2, y)] : f[idx(w_ - 2, y)];
        }
        // The corners have no single neighbor to mirror, so they average the two beside them.
        f[idx(0, 0)]           = (f[idx(1, 0)] + f[idx(0, 1)]) / 2;
        f[idx(w_ - 1, 0)]      = (f[idx(w_ - 2, 0)] + f[idx(w_ - 1, 1)]) / 2;
        f[idx(0, h_ - 1)]      = (f[idx(1, h_ - 1)] + f[idx(0, h_ - 2)]) / 2;
        f[idx(w_ - 1, h_ - 1)] = (f[idx(w_ - 2, h_ - 1)] + f[idx(w_ - 1, h_ - 2)]) / 2;
    }

    /// Gauss-Seidel relaxation: the shared inner loop of both diffuse and project. Each cell
    /// becomes a weighted average of itself and its four neighbors, repeated until it settles.
    void relax(int32_t* f, const int32_t* f0, int32_t a, int32_t c, uint8_t iters, int b) {
        if (c == 0) return;
        for (uint8_t k = 0; k < iters; k++) {
            for (lengthType y = 1; y < h_ - 1; y++) {
                for (lengthType x = 1; x < w_ - 1; x++) {
                    const int64_t neigh = static_cast<int64_t>(f[idx(x - 1, y)]) + f[idx(x + 1, y)]
                                        + f[idx(x, y - 1)] + f[idx(x, y + 1)];
                    // 64-bit for the product: a Q16.16 velocity times a Q16.16 coefficient is a
                    // Q32.32 intermediate, and truncating it to 32 bits loses the whole integer
                    // part on any but the slowest flow.
                    const int64_t v = (static_cast<int64_t>(f0[idx(x, y)]) << 16)
                                    + static_cast<int64_t>(a) * neigh;
                    f[idx(x, y)] = static_cast<int32_t>(v / c);
                }
            }
            setBoundary(f, b);
        }
    }

    void diffuse(int32_t* f, int32_t* f0, int32_t visc, int32_t dt, uint8_t iters, int b) {
        std::memcpy(f0, f, plane() * sizeof(int32_t));
        const int64_t a = (static_cast<int64_t>(visc) * dt) >> 16;
        if (a <= 0) return;
        relax(f, f0, static_cast<int32_t>(a), static_cast<int32_t>((kOne + 4 * a)), iters, b);
    }

    /// Make the field divergence-free: compute how much each cell is gaining or losing, solve for
    /// a pressure whose gradient cancels it, then subtract that gradient.
    ///
    /// This is the step that separates a fluid from a field of arrows. Without it the medium piles
    /// up in some places and drains from others, and anything carried by it clumps and vanishes.
    void project(int32_t* vx, int32_t* vy, uint8_t iters) {
        int32_t* div = div_.data();
        int32_t* p = p_.data();
        for (lengthType y = 1; y < h_ - 1; y++)
            for (lengthType x = 1; x < w_ - 1; x++) {
                const int64_t d = static_cast<int64_t>(vx[idx(x + 1, y)]) - vx[idx(x - 1, y)]
                                + vy[idx(x, y + 1)] - vy[idx(x, y - 1)];
                div[idx(x, y)] = static_cast<int32_t>(-d / 2);
                p[idx(x, y)] = 0;
            }
        setBoundary(div, 0);
        setBoundary(p, 0);
        relax(p, div, kOne, 4 * kOne, iters, 0);
        for (lengthType y = 1; y < h_ - 1; y++)
            for (lengthType x = 1; x < w_ - 1; x++) {
                vx[idx(x, y)] -= (p[idx(x + 1, y)] - p[idx(x - 1, y)]) / 2;
                vy[idx(x, y)] -= (p[idx(x, y + 1)] - p[idx(x, y - 1)]) / 2;
            }
        setBoundary(vx, 1);
        setBoundary(vy, 2);
    }

    /// The velocity carries itself: backward-sampled, like every other advection here, because
    /// that is what stays stable when a cell would otherwise move further than one cell per step.
    void advectSelf(int32_t* vx, int32_t* vy, const int32_t* vx0, const int32_t* vy0, int32_t dt) {
        for (lengthType y = 1; y < h_ - 1; y++) {
            for (lengthType x = 1; x < w_ - 1; x++) {
                // Where this cell's contents came from, in Q16.16 cell coordinates.
                int64_t sx = (static_cast<int64_t>(x) << 16) - ((static_cast<int64_t>(vx0[idx(x, y)]) * dt) >> 16);
                int64_t sy = (static_cast<int64_t>(y) << 16) - ((static_cast<int64_t>(vy0[idx(x, y)]) * dt) >> 16);
                // Clamped half a cell inside the wall, so the bilinear read below always has four
                // real neighbors and the boundary handles the rest.
                const int64_t lo = kOne / 2, hiX = (static_cast<int64_t>(w_) << 16) - kOne - kOne / 2;
                const int64_t hiY = (static_cast<int64_t>(h_) << 16) - kOne - kOne / 2;
                sx = sx < lo ? lo : (sx > hiX ? hiX : sx);
                sy = sy < lo ? lo : (sy > hiY ? hiY : sy);
                const lengthType x0 = static_cast<lengthType>(sx >> 16), y0 = static_cast<lengthType>(sy >> 16);
                const int64_t fx = sx & 0xFFFF, fy = sy & 0xFFFF;
                const auto blend = [&](const int32_t* f) -> int32_t {
                    const int64_t a = f[idx(x0, y0)] + (((static_cast<int64_t>(f[idx(x0 + 1, y0)]) - f[idx(x0, y0)]) * fx) >> 16);
                    const int64_t b = f[idx(x0, y0 + 1)] + (((static_cast<int64_t>(f[idx(x0 + 1, y0 + 1)]) - f[idx(x0, y0 + 1)]) * fx) >> 16);
                    return static_cast<int32_t>(a + (((b - a) * fy) >> 16));
                };
                vx[idx(x, y)] = blend(vx0);
                vy[idx(x, y)] = blend(vy0);
            }
        }
        setBoundary(vx, 1);
        setBoundary(vy, 2);
    }

    ScratchBuffer<int32_t> vx_, vy_;      ///< the velocity field, Q16.16
    ScratchBuffer<int32_t> vx0_, vy0_;    ///< the previous field, which advection reads
    ScratchBuffer<int32_t> p_, div_;      ///< the pressure solve's working grids
    lengthType w_ = 0, h_ = 0, d_ = 0;
    size_t     cells_ = 0;          ///< every slice together
};

}  // namespace mm
