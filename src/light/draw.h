#pragma once

#include "light/light_types.h"   // Coord3D, lengthType
#include "light/layers/Buffer.h" // Buffer (flat light array)
#include "core/color.h"          // RGB, scale8
#include "core/math8.h"          // qadd8 — saturating add for blur's seep accumulation
#include "light/Palette.h"       // blend(RGB,RGB,amt) — for blendPixel
#include "light/fonts.h"         // fonts::Font — bitmap glyph tables for draw::text

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

// Add a colour into a pixel, saturating (a bright pixel can't wrap to dark) — WLED's addRGB / additive
// setPixelColor. Used to re-stamp a light on top of a blur so its centre stays bright. Clipped like pixel().
inline void addPixel(Buffer& buf, Coord3D dims, Coord3D p, RGB c) {
    const size_t off = offsetOf(buf, dims, p);
    if (off + 2 >= buf.bytes()) return;
    uint8_t* d = buf.data();
    d[off + 0] = qadd8(d[off + 0], c.r);
    d[off + 1] = qadd8(d[off + 1], c.g);
    d[off + 2] = qadd8(d[off + 2], c.b);
}

// Fade the whole buffer toward black by amt/255 — one pass over the bytes. This is the primitive the
// Layer's once-per-frame collected fade (Layer::fadeToBlackBy) applies; effects request a fade through
// the Layer (which MINs the amount across effects and calls this once) rather than calling it directly.
inline void fade(Buffer& buf, uint8_t amt) {
    const uint8_t keep = static_cast<uint8_t>(255 - amt);
    uint8_t* d = buf.data();
    const size_t n = buf.bytes();
    for (size_t i = 0; i < n; i++) d[i] = scale8(d[i], keep);
}

// Box blur, working 1D→3D against the flat Buffer — one unified primitive, not a blur1d/blur2d/blur3d
// trio (the *common patterns first* / "primitives are 3D-aware" rule, same as draw::line). It runs a
// separable seep pass along each axis whose extent is >1: a 1×N 1D layer blurs along y (its only
// axis with extent>1); 2D along x then y; 3D along x, y, z. `amt` (0 = none, 255 = max) is split
// keep=255-amt / seep=amt>>1 per pixel.
//
// Algorithm: the canonical FastLED blur1d single-forward-pass with carryover — each pixel keeps
// `keep` of itself, seeps `seep` forward to the next pixel and `seep` back to the previous one, so
// one O(N) pass per axis approximates a symmetric box blur. Behaviour is identical to MoonLight's
// blur1d/blurRows/blurColumns (verified against VirtualLayer.cpp); the speed comes from doing it on
// the raw bytes — a stride walk with three uint8 carried in registers, no per-pixel getRGB/setRGB/
// Coord3D construction (the overhead that makes a generic-layer blur an FPS killer). Prior art:
// FastLED's blur1d (Mark Kriegsman), the recognisable carryover-seep; our byte-level implementation.
//
// `stride` is the byte step between adjacent pixels ALONG the blurred axis (cpl for x, w·cpl for y,
// w·h·cpl for z); `lineCount`/`lineStride` walk the starts of each parallel line. RGB only (first 3
// channels); a W channel is untouched. Saturating adds (qadd8) so a bright pixel can't wrap to dark.
inline void blurAxis(uint8_t* d, size_t cpl, size_t len, size_t stride,
                     size_t lineCount, size_t lineStride, uint8_t amt) {
    if (len < 2 || cpl < 3) return;                 // nothing to seep along a 1-pixel (or sub-RGB) axis
    const uint8_t keep = static_cast<uint8_t>(255 - amt);
    const uint8_t seep = static_cast<uint8_t>(amt >> 1);
    for (size_t l = 0; l < lineCount; l++) {
        uint8_t* base = d + l * lineStride;
        uint8_t cr = 0, cg = 0, cb = 0;             // carryover (the seep flowing forward), starts black
        size_t off = 0, prev = 0;
        for (size_t i = 0; i < len; i++, off += stride) {
            uint8_t* px = base + off;
            const uint8_t pr = scale8(px[0], seep), pg = scale8(px[1], seep), pb = scale8(px[2], seep);
            px[0] = qadd8(scale8(px[0], keep), cr);  // keep self + receive prev pixel's forward seep
            px[1] = qadd8(scale8(px[1], keep), cg);
            px[2] = qadd8(scale8(px[2], keep), cb);
            if (i) {                                 // seep back into the previous pixel (deferred add)
                uint8_t* pv = base + prev;
                pv[0] = qadd8(pv[0], pr); pv[1] = qadd8(pv[1], pg); pv[2] = qadd8(pv[2], pb);
            }
            cr = pr; cg = pg; cb = pb; prev = off;
        }
        uint8_t* last = base + prev;                 // the final forward seep lands on the last pixel
        last[0] = qadd8(last[0], cr); last[1] = qadd8(last[1], cg); last[2] = qadd8(last[2], cb);
    }
}

// Blur the whole buffer by `amt`, separably along every axis with extent >1 (x, then y, then z —
// MoonLight's blur2d order, extended to z). One call covers 1D/2D/3D. Off the per-pixel-effect path.
inline void blur(Buffer& buf, Coord3D dims, uint8_t amt) {
    if (amt == 0) return;
    uint8_t* d = buf.data();
    const size_t cpl = buf.channelsPerLight();
    const size_t w = dims.x > 0 ? static_cast<size_t>(dims.x) : 0;
    const size_t h = dims.y > 0 ? static_cast<size_t>(dims.y) : 0;
    const size_t z = dims.z > 0 ? static_cast<size_t>(dims.z) : 0;
    if (w == 0 || h == 0 || z == 0) return;
    if (static_cast<size_t>(w * h * z) * cpl > buf.bytes()) return;   // dims/buffer mismatch guard
    // x-pass: each (y,z) line is `w` pixels, stride cpl; lines start every w·cpl bytes, h·z of them.
    blurAxis(d, cpl, w, cpl, h * z, w * cpl, amt);
    // y-pass: each (x,z) line is `h` pixels, stride w·cpl. Lines: for each z, the w columns — start
    // offsets are z·(h·w·cpl) + x·cpl. Walk them as one run of (w·z) lines stepping by cpl, but the
    // z blocks aren't contiguous in column-start, so loop z outside.
    for (size_t zz = 0; zz < z; zz++)
        blurAxis(d + zz * h * w * cpl, cpl, h, w * cpl, w, cpl, amt);
    // z-pass (3D only): each (x,y) line is `z` pixels, stride w·h·cpl; w·h lines stepping by cpl.
    if (z > 1) blurAxis(d, cpl, z, w * h * cpl, w * h, cpl, amt);
}

// Fill the whole buffer with one colour (MoonLight's fill_solid).
inline void fill(Buffer& buf, RGB c) {
    const uint8_t cpl = buf.channelsPerLight();
    if (cpl == 0) return;   // a 0-channel buffer has no colour to write; guards off += 0 spinning
    uint8_t* d = buf.data();
    const size_t n = buf.bytes();
    for (size_t off = 0; off + cpl <= n; off += cpl) {
        if (cpl >= 1) d[off + 0] = c.r;
        if (cpl >= 2) d[off + 1] = c.g;
        if (cpl >= 3) d[off + 2] = c.b;
    }
}

// Blit one glyph of `font` at grid position (x, y). Each of the font's `height` rows is one byte;
// bit set → pixel on, columns MSB-first across the glyph's `width` (bit (7) is the left column, down
// to bit (8-width)). Only printable ASCII 32..126 is drawn; anything else is skipped (a gap). Pixels
// are clipped to the grid (draw::pixel). Prior art: the WLED/MoonLight console-font blitter shape.
inline void glyph(Buffer& buf, Coord3D dims, const fonts::Font& font, char ch, lengthType x, lengthType y, RGB c) {
    if (ch < 32 || ch > 126) return;
    const uint8_t idx = static_cast<uint8_t>(ch - 32);
    const uint8_t* rows = font.rows + static_cast<size_t>(idx) * font.height;
    for (uint8_t ry = 0; ry < font.height; ry++) {
        const uint8_t bits = rows[ry];
        // Columns are MSB-first: the LEFTMOST glyph column (rx=0) is bit 7, the next bit 6, … so
        // read column rx from bit (7 - rx). (Reading (rx + 8-width) instead mirrors each glyph
        // left-to-right — a 'b' renders as a 'd'.)
        for (uint8_t rx = 0; rx < font.width; rx++)
            if ((bits >> (7 - rx)) & 0x01)
                pixel(buf, dims, {static_cast<lengthType>(x + rx), static_cast<lengthType>(y + ry), 0}, c);
    }
}

// Draw a NUL-terminated string starting at (x, y), advancing `font.width` per character. `\n` starts
// a new line one `font.height` down and resets x to the start column (multi-line layout). Off-grid
// glyphs clip. Returns the total pixel width drawn on the first line (for scroll bookkeeping).
inline lengthType text(Buffer& buf, Coord3D dims, const fonts::Font& font, const char* str,
                       lengthType x, lengthType y, RGB c) {
    if (!str) return 0;
    lengthType cx = x, cy = y;
    lengthType firstLineWidth = 0;   // frozen at the first '\n' so a multi-line string still reports line 1
    bool onFirstLine = true;
    for (const char* p = str; *p; p++) {
        if (*p == '\n') {
            if (onFirstLine) { firstLineWidth = static_cast<lengthType>(cx - x); onFirstLine = false; }
            cx = x; cy = static_cast<lengthType>(cy + font.height); continue;
        }
        glyph(buf, dims, font, *p, cx, cy, c);
        cx = static_cast<lengthType>(cx + font.width);
    }
    return onFirstLine ? static_cast<lengthType>(cx - x) : firstLineWidth;
}

}  // namespace draw
}  // namespace mm
