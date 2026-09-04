#pragma once

#include "core/math16.h"              // atan16, dist16, kaleido, BeatPhase
#include "core/noise.h"               // fbm8: the tunnel wall texture
#include "light/effects/EffectBase.h"
#include "light/polar.h"              // PolarLut: the per-pixel angle and radius, precomputed

namespace mm {

// Tunnel: the demoscene classic — a texture mapped onto the inside of an infinite tube, so the
// viewer appears to fly down it forever.
//
// The trick is that nothing is 3D. For each pixel, its ANGLE around the centre becomes one texture
// coordinate and the RECIPROCAL of its distance becomes the other. Because 1/r grows without bound
// as you approach the centre, the texture compresses toward a vanishing point, and adding time to
// that coordinate pulls it toward the viewer. Perspective for the price of a divide.
//
// It is the polar vocabulary carried to its conclusion: `atan16` and `dist16` give the angle and
// radius, and the perspective is one reciprocal on top of them. (For the GATHER primitive —
// reading the grid itself as a texture — see EchoEffect, which feeds a transformed previous frame
// back through `sampleWrap`.)
//
// Cost: one divide, one atan and one noise sample per pixel. The divide is the expensive part on a
// chip without hardware division; `depth` sets how fine the wall texture is, not how much it costs.
//
// Prior art: the standard demoscene tunnel (angle + 1/r texture mapping), and Iñigo Quilez's
// write-ups of it. Implemented fresh in fixed point.
// @card TunnelEffect.png
/// Effect: a texture-mapped tunnel flying toward a vanishing point.
class TunnelEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D2; }  // writes the z=0 slice; extrude fills z

    uint8_t bpm      = 20;   // how fast the tunnel flies past
    uint8_t depth    = 60;   // texture scale along the tunnel: higher = finer rings
    uint8_t twist    = 40;   // rotation per unit depth, so the tunnel corkscrews
    uint8_t segments = 1;    // kaleidoscope the wall; 1 leaves it plain
    uint8_t octaves  = 2;    // wall texture detail, and the cost knob
    bool    vignette = true; // darken toward the vanishing point so it reads as receding

    /// Read the polar address from a table instead of computing it per pixel (light/polar.h). It
    /// costs 2 bytes per pixel, 4 when wide, and the effect falls back to computing the address
    /// when the device cannot spare them.
    bool usePolarTable = true;
    bool widePolarTable = false;

    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 120);
        controls_.addControl("depth", depth, 1, 255);
        controls_.addControl("twist", twist, 0, 255);
        controls_.addControl("segments", segments, 1, 16);
        controls_.addControl("octaves", octaves, 1, 4);
        controls_.addControl("vignette", vignette);
        controls_.addControl("polarTable", usePolarTable);
        controls_.addControl("polarTable16", widePolarTable);
    }
    void prepare() override {
        // The polar address is built here, not in tick(): prepare() is where a module builds state
        // and where allocation is allowed, and it runs again on every resize and control change, so
        // the table is always current without the render path ever allocating.
        if (usePolarTable) lut_.prepare(static_cast<uint16_t>(width()), static_cast<uint16_t>(height()), widePolarTable);
        else               lut_.release();
    }


    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();

        phase_.advance(elapsed(), bpm);
        const uint32_t t = phase_.phase(65536);

        const int32_t cx = w / 2, cy = h / 2;

        // The polar address does not change between frames, so it is read from a table; if the
        // device cannot spare the memory the effect computes it per pixel and looks the same.
        const bool table = lut_.ready();

        std::size_t i = 0;
        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++, i++) {
                uint32_t r;
                angle16 a;
                if (table) {
                    a = lut_.angle(i);
                    r = lut_.radiusPixels(i);
                } else {
                    const int32_t dx = static_cast<int32_t>(x) - cx;
                    const int32_t dy = static_cast<int32_t>(y) - cy;
                    r = dist16(dx, dy);
                    a = atan16(dy, dx);
                }

                // 1/r is the depth coordinate: distant wall (small r) compresses toward the centre,
                // which is exactly the perspective foreshortening a real tunnel has. The +1 keeps
                // the pixel at the very centre from dividing by zero.
                const uint32_t depthCoord = (static_cast<uint32_t>(depth) * 4096u) / (r + 1);

                // The wall corkscrews: rotating by depth means each ring is turned a little more
                // than the one behind it.
                a = static_cast<angle16>(a + ((depthCoord * twist) >> 6));
                a = kaleido(a, segments);

                // Sample the wall texture in (angle, depth) space, with time pulling the depth
                // coordinate toward the viewer.
                const uint32_t u = (static_cast<uint32_t>(a) >> 5);
                const uint32_t v = depthCoord + (t >> 5);
                const uint8_t tex = fbm8(u, v, octaves);

                // Vignette by distance so the centre reads as far away rather than merely small.
                uint8_t bri = 255;
                if (vignette) {
                    const uint32_t maxR = static_cast<uint32_t>(cx > cy ? cx : cy) + 1;
                    const uint32_t rel = r > maxR ? 255u : (r * 255u) / maxR;
                    bri = static_cast<uint8_t>(rel < 20 ? 20 : rel);
                }

                draw::pixel(cv, {x, y, 0}, colorFromPalette(*Palettes::active(), tex, bri));
            }
        }
    }

private:
    PolarLut  lut_{*this};
    BeatPhase phase_;
};

}  // namespace mm
