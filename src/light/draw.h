#pragma once

#include "light/light_types.h"   // Coord3D, lengthType
#include "light/layers/Buffer.h" // Buffer (flat light array)
#include "core/color.h"          // RGB, scale8
#include "light/Palette.h"       // blend(RGB,RGB,amt) — for blendPixel

// Geometry draw primitives for effects/modifiers: set a pixel, draw a line — bounds-clipped,
// integer-only, working 1D→3D against the flat light Buffer. The "core absorbs the hard part"
// rule applied to drawing: the Bresenham + clipping lives here once, so an effect calls
// drawLine() instead of re-rolling it. Light-domain (it touches the light Buffer), not core.
//
// Prior art: the line algorithm is Bresenham (1962) generalised to 3D (the textbook DDA-error
// form). FastLED keeps draw in its 2D/matrix add-ons, not core — same split here.
//
// The Buffer is a flat array of `count` lights × `cpl` channels; the grid SHAPE (w,h,d) lives on
// the Layer/Layout, so the caller passes `dims` (the Coord3D extent). Index order matches the
// engine: off = (z·h·w + y·w + x)·cpl. A pixel outside [0,w)×[0,h)×[0,d) is silently clipped, so
// a line that runs off the grid just stops drawing — no out-of-bounds write (the robustness rule).

namespace mm {
namespace draw {

// One pixel, clipped to the grid. Writes R/G/B where channels fit (cpl may be 1..N); extra
// channels (e.g. a W in RGBW) are left as-is — the driver derives white, same as effects do.
inline void pixel(Buffer& buf, Coord3D dims, Coord3D p, RGB c) {
    if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= dims.x || p.y >= dims.y || p.z >= dims.z) return;
    const uint8_t cpl = buf.channelsPerLight();
    const size_t off = (static_cast<size_t>(p.z) * dims.y * dims.x
                        + static_cast<size_t>(p.y) * dims.x + p.x) * cpl;
    if (off + (cpl < 3 ? cpl : 3) > buf.bytes()) return;   // defends a dims/buffer mismatch
    uint8_t* d = buf.data();
    if (cpl >= 1) d[off + 0] = c.r;
    if (cpl >= 2) d[off + 1] = c.g;
    if (cpl >= 3) d[off + 2] = c.b;
}

// A straight line a→b, clipped to the grid. 3D Bresenham: step along the dominant axis and carry
// an integer error term per other axis (the textbook generalisation of the 2D line). Works for
// 1D (a row), 2D (a plane), and 3D (a volume) without special-casing — a degenerate axis just
// never steps. Endpoints are inclusive.
//
// `shorten` (0..255, default 255 = full line) draws only the first shorten/255 of the way from a
// toward b — the far endpoint is pulled back toward `a`. 255 = whole line, 128 ≈ half, 1 = the
// start pixel, 0 = nothing. This is the perspective/length lever (MoonLight's `depth` param):
// effects animate the drawn tip by varying `shorten`, so a fixed pair of endpoints traces a
// sweeping partial segment over successive frames. (WLEDMM's *2-rounding shorten, generalised 3D.)
inline void line(Buffer& buf, Coord3D dims, Coord3D a, Coord3D b, RGB c, uint8_t shorten = 255) {
    if (shorten == 0) return;
    if (shorten < 255) {
        // Pull b back toward a by shorten/255, with *2 rounding like WLEDMM.
        const int bx = ((2 * int(b.x) - 2 * int(a.x)) * int(shorten)) / 255 + 2 * int(a.x);
        const int by = ((2 * int(b.y) - 2 * int(a.y)) * int(shorten)) / 255 + 2 * int(a.y);
        const int bz = ((2 * int(b.z) - 2 * int(a.z)) * int(shorten)) / 255 + 2 * int(a.z);
        b = {static_cast<lengthType>((bx + 1) / 2),
             static_cast<lengthType>((by + 1) / 2),
             static_cast<lengthType>((bz + 1) / 2)};
    }
    Coord3D p = a;
    const lengthType dx = b.x > a.x ? static_cast<lengthType>(b.x - a.x) : static_cast<lengthType>(a.x - b.x);
    const lengthType dy = b.y > a.y ? static_cast<lengthType>(b.y - a.y) : static_cast<lengthType>(a.y - b.y);
    const lengthType dz = b.z > a.z ? static_cast<lengthType>(b.z - a.z) : static_cast<lengthType>(a.z - b.z);
    const lengthType sx = b.x >= a.x ? 1 : -1;
    const lengthType sy = b.y >= a.y ? 1 : -1;
    const lengthType sz = b.z >= a.z ? 1 : -1;

    // Drive the loop off the longest axis; accumulate error toward the other two.
    if (dx >= dy && dx >= dz) {
        lengthType ey = static_cast<lengthType>(dx / 2), ez = ey;
        for (;; p.x = static_cast<lengthType>(p.x + sx)) {
            pixel(buf, dims, p, c);
            if (p.x == b.x) break;
            if ((ey = static_cast<lengthType>(ey - dy)) < 0) { ey = static_cast<lengthType>(ey + dx); p.y = static_cast<lengthType>(p.y + sy); }
            if ((ez = static_cast<lengthType>(ez - dz)) < 0) { ez = static_cast<lengthType>(ez + dx); p.z = static_cast<lengthType>(p.z + sz); }
        }
    } else if (dy >= dz) {
        lengthType ex = static_cast<lengthType>(dy / 2), ez = ex;
        for (;; p.y = static_cast<lengthType>(p.y + sy)) {
            pixel(buf, dims, p, c);
            if (p.y == b.y) break;
            if ((ex = static_cast<lengthType>(ex - dx)) < 0) { ex = static_cast<lengthType>(ex + dy); p.x = static_cast<lengthType>(p.x + sx); }
            if ((ez = static_cast<lengthType>(ez - dz)) < 0) { ez = static_cast<lengthType>(ez + dy); p.z = static_cast<lengthType>(p.z + sz); }
        }
    } else {
        lengthType ex = static_cast<lengthType>(dz / 2), ey = ex;
        for (;; p.z = static_cast<lengthType>(p.z + sz)) {
            pixel(buf, dims, p, c);
            if (p.z == b.z) break;
            if ((ex = static_cast<lengthType>(ex - dx)) < 0) { ex = static_cast<lengthType>(ex + dz); p.x = static_cast<lengthType>(p.x + sx); }
            if ((ey = static_cast<lengthType>(ey - dy)) < 0) { ey = static_cast<lengthType>(ey + dz); p.y = static_cast<lengthType>(p.y + sy); }
        }
    }
}

// --- Buffer read/modify helpers --------------------------------------------
// The offset of a pixel in the flat buffer, or buf.bytes() if out of bounds (caller checks).
inline size_t offsetOf(const Buffer& buf, Coord3D dims, Coord3D p) {
    if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= dims.x || p.y >= dims.y || p.z >= dims.z) return buf.bytes();
    return (static_cast<size_t>(p.z) * dims.y * dims.x + static_cast<size_t>(p.y) * dims.x + p.x)
           * buf.channelsPerLight();
}

// Read the RGB at a pixel (black if out of bounds / fewer than 3 channels).
inline RGB get(const Buffer& buf, Coord3D dims, Coord3D p) {
    const size_t off = offsetOf(buf, dims, p);
    if (off + 2 >= buf.bytes()) return {0, 0, 0};
    const uint8_t* d = buf.data();
    return {d[off + 0], d[off + 1], d[off + 2]};
}

// Blend a colour into a pixel by amt/255 (amt 0 = leave as-is, 255 = replace). The in-place
// read-modify-write that GoL's dead-cell fade-to-background and age-toward-red use
// (MoonLight's blendColor). Clipped like pixel().
inline void blendPixel(Buffer& buf, Coord3D dims, Coord3D p, RGB c, uint8_t amt) {
    const size_t off = offsetOf(buf, dims, p);
    if (off + 2 >= buf.bytes()) return;
    uint8_t* d = buf.data();
    const RGB cur{d[off + 0], d[off + 1], d[off + 2]};
    const RGB out = blend(cur, c, amt);
    d[off + 0] = out.r; d[off + 1] = out.g; d[off + 2] = out.b;
}

// Fade the whole buffer toward black by amt/255 (MoonLight's layer fadeToBlackBy). Hot-ish but
// off the per-pixel-effect path — one pass over the bytes.
inline void fade(Buffer& buf, uint8_t amt) {
    const uint8_t keep = static_cast<uint8_t>(255 - amt);
    uint8_t* d = buf.data();
    const size_t n = buf.bytes();
    for (size_t i = 0; i < n; i++) d[i] = scale8(d[i], keep);
}

// Fill the whole buffer with one colour (MoonLight's fill_solid).
inline void fill(Buffer& buf, RGB c) {
    const uint8_t cpl = buf.channelsPerLight();
    uint8_t* d = buf.data();
    const size_t n = buf.bytes();
    for (size_t off = 0; off + cpl <= n; off += cpl) {
        if (cpl >= 1) d[off + 0] = c.r;
        if (cpl >= 2) d[off + 1] = c.g;
        if (cpl >= 3) d[off + 2] = c.b;
    }
}

}  // namespace draw
}  // namespace mm
