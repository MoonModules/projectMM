#pragma once

#include "core/util/ScratchBuffer.h"   // the grids an effect owns
#include "light/util/light_types.h"

#include <cstring>

namespace mm {

/// A stable-fluid velocity field: the medium itself, simulated rather than sampled.
///
/// Its velocity is state that evolves from its own past, so a vortex forms because the maths says it must.
/// Stam's solver, in fixed point because the render path is integer.
///
/// The four-step frame order and why sixteen fraction bits: power-functions.md#the-fluid-solver
class Fluid {
public:
    explicit Fluid(MoonModule& owner)
        : vx_(owner), vy_(owner), vx0_(owner), vy0_(owner), p_(owner), div_(owner) {}

    /// Q16.16: the fixed-point format the whole solver works in.
    static constexpr int32_t kOne = 1 << 16;

    /// Size (or free) the grids. Returns whether a field is available, which is what an effect reports.
    bool resize(lengthType w, lengthType h, lengthType d = 1) {
        if (w <= 2 || h <= 2 || d < 1) { release(); return false; }   // a grid with no interior
        const size_t n = static_cast<size_t>(w) * h * d;
        // Both this shape and still allocated: a freed buffer would otherwise report a grid that is gone.
        if (w == w_ && h == h_ && d == d_ && vx_) return true;
        // The working grids are per slice: sizing them to the volume cost 121 KB of unused copies on a 20-cube.
        const size_t slice = static_cast<size_t>(w) * h;
        const bool ok = vx_.resize(n) && vy_.resize(n) && vx0_.resize(slice)
                     && vy0_.resize(slice) && p_.resize(slice) && div_.resize(slice);
        if (!ok) { release(); return false; }
        w_ = w; h_ = h; d_ = d; cells_ = n;
        reset();
        return true;
    }

    /// Drop every field, returning the solver to its unsized state.
    void release() {
        vx_.resize(0); vy_.resize(0); vx0_.resize(0); vy0_.resize(0); p_.resize(0); div_.resize(0);
        w_ = h_ = d_ = 0; cells_ = 0;
    }

    /// Re-seed to rest: every velocity zero. The resync point, for a fixture that has been reconfigured under a running simulation.
    void reset() {
        if (!valid()) return;
        std::memset(vx_.data(), 0, vx_.bytes());
        std::memset(vy_.data(), 0, vy_.bytes());
    }

    /// Ready to step. Read from the buffers, not from a cached shape.
    bool valid() const { return cells_ > 0 && vx_ && vy_ && vx0_ && vy0_ && p_ && div_; }
    /// The grid's width in cells.
    lengthType width() const { return w_; }
    /// Its height in cells.
    lengthType height() const { return h_; }
    /// How many slices it carries.
    lengthType depth() const { return d_; }
    /// Cells per slice: a slice's fields start at `z * plane()` in velocityX()/velocityY().
    size_t plane() const { return static_cast<size_t>(w_) * h_; }
    /// The x velocity field, one Q16.16 value per cell.
    const int32_t* velocityX() const { return vx_.data(); }
    /// The y velocity field, laid out the same way.
    const int32_t* velocityY() const { return vy_.data(); }

    /// Push the medium at one cell. The source term: an emitter, a control, a beat.
    void addVelocity(lengthType x, lengthType y, int32_t dvx, int32_t dvy, lengthType z = 0) {
        if (!valid() || x < 0 || y < 0 || z < 0 || x >= w_ || y >= h_ || z >= d_) return;
        const size_t i = static_cast<size_t>(z) * plane() + idx(x, y);
        vx_[i] += dvx;
        vy_[i] += dvy;
    }

    /// One frame of the simulation. `viscosity` and `dt` are Q16.16.
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

    /// Gauss-Seidel relaxation.
    void relax(int32_t* f, const int32_t* f0, int32_t a, int32_t c, uint8_t iters, int b) {
        if (c == 0) return;
        for (uint8_t k = 0; k < iters; k++) {
            for (lengthType y = 1; y < h_ - 1; y++) {
                for (lengthType x = 1; x < w_ - 1; x++) {
                    const int64_t neigh = static_cast<int64_t>(f[idx(x - 1, y)]) + f[idx(x + 1, y)]
                                        + f[idx(x, y - 1)] + f[idx(x, y + 1)];
                    // Widened for the product: truncating it loses the integer part on any but the slowest flow.
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

    /// Make the field divergence-free.
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

    /// The velocity carries itself.
    void advectSelf(int32_t* vx, int32_t* vy, const int32_t* vx0, const int32_t* vy0, int32_t dt) {
        for (lengthType y = 1; y < h_ - 1; y++) {
            for (lengthType x = 1; x < w_ - 1; x++) {
                // Where this cell's contents came from, in Q16.16 cell coordinates.
                int64_t sx = (static_cast<int64_t>(x) << 16) - ((static_cast<int64_t>(vx0[idx(x, y)]) * dt) >> 16);
                int64_t sy = (static_cast<int64_t>(y) << 16) - ((static_cast<int64_t>(vy0[idx(x, y)]) * dt) >> 16);
                // Clamped half a cell inside the wall, so the bilinear read below always has four real neighbors and the boundary handles the rest.
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
