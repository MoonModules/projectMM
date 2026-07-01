#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/draw.h"           // draw::pixel, draw::fade, draw::offsetOf
#include "light/light_types.h"    // Coord3D, lengthType
#include "core/color.h"           // RGB

namespace mm {

// Fixed Rectangle: paints a solid axis-aligned RGB box at a fixed grid position and extent,
// over a slow motion-trail fade. The box origin (X/Y/Z position) and size (width/height/depth)
// are plain controls, so the user dials in exactly which cells light up — a static fixture /
// alignment / region paint. When `alternateWhite` is on, the box is rendered as a chequerboard
// of white and the RGB colour: a per-cell toggle flips along the box's dominant axis (it flips
// every cell when the box is wider than tall, and once per row when it is taller than wide), so
// the white/colour pattern follows the box's longer side. The 4th (white) channel is written
// only on RGBW grids.
//
// Prior art: MoonLight's FixedRectangle (E_MoonModules / MoonModules). The defaults, the
// origin+extent clamping, the alternate-toggle rule (flip per-cell when height<width, flip
// per-row when height>width), and the white-vs-colour chequerboard are reproduced exactly,
// written fresh on EffectBase + the shared draw primitives.
class FixedRectangleEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin
    Dim dimensions() const override { return Dim::D3; }

    // Colour of the box (MoonLight defaults). White is the 4th-channel value, used only on RGBW.
    uint8_t red   = 182;
    uint8_t green = 15;
    uint8_t blue  = 98;
    uint8_t white = 0;

    // Box origin (min 0) and extent (min 1). Named rect* to avoid hiding the inherited
    // width()/height()/depth() grid accessors; the UI control names carry the source's labels.
    int16_t rectX = 0, rectY = 0, rectZ = 0;
    int16_t rectW = 1, rectH = 1, rectD = 1;

    bool alternateWhite = false;

    void onBuildControls() override {
        controls_.addUint8("red",   red);
        controls_.addUint8("green", green);
        controls_.addUint8("blue",  blue);
        controls_.addUint8("white", white);
        controls_.addInt16("X position", rectX, 0, INT16_MAX);
        controls_.addInt16("Y position", rectY, 0, INT16_MAX);
        controls_.addInt16("Z position", rectZ, 0, INT16_MAX);
        controls_.addInt16("Rectangle width",  rectW, 1, INT16_MAX);
        controls_.addInt16("Rectangle height", rectH, 1, INT16_MAX);
        controls_.addInt16("Rectangle depth",  rectD, 1, INT16_MAX);
        controls_.addBool("alternateWhite", alternateWhite);
    }

    void loop() override {
        const int w = width();
        const int h = height();
        const int d = depth();
        if (w <= 0 || h <= 0) return;

        Buffer& buf = layer()->buffer();
        const uint8_t cpl = channelsPerLight();
        if (cpl < 3) return;
        const Coord3D dims{static_cast<lengthType>(w), static_cast<lengthType>(h), depthDim()};

        // Motion trail: dim the whole buffer each frame (source: layer->fadeToBlackBy(10)).
        draw::fade(buf, 10);

        // The white/colour chequerboard toggle. MoonLight keeps it as a member but resets it to
        // false at the top of each draw, so it is effectively per-frame state — a plain local here.
        bool alternate = false;

        const RGB rgb{red, green, blue};
        const bool anyColor = (red || green || blue);

        // Iterate the box clamped to the live grid (origin + extent, capped at each axis size).
        const int zEnd = MINi(rectZ + rectD, d > 0 ? d : 1);
        const int yEnd = MINi(rectY + rectH, h);
        const int xEnd = MINi(rectX + rectW, w);

        for (int z = rectZ; z < zEnd; z++) {
            for (int y = rectY; y < yEnd; y++) {
                for (int x = rectX; x < xEnd; x++) {
                    if (anyColor) {
                        const RGB c = (alternateWhite && alternate) ? RGB{255, 255, 255} : rgb;
                        draw::pixel(buf, dims, {static_cast<lengthType>(x), static_cast<lengthType>(y), static_cast<lengthType>(z)}, c);
                    }
                    // White channel: only on RGBW grids (4th channel). draw::pixel writes RGB only.
                    if (white > 0 && cpl >= 4) {
                        const size_t off = draw::offsetOf(buf, dims, {static_cast<lengthType>(x), static_cast<lengthType>(y), static_cast<lengthType>(z)});
                        if (off + 3 < buf.bytes()) buf.data()[off + 3] = white;
                    }
                    // Box wider than tall: flip the white/colour toggle every cell along X.
                    if (rectH < rectW) alternate = !alternate;
                }
                // Box taller than wide: flip once per row instead.
                if (rectH > rectW) alternate = !alternate;
            }
        }
    }

private:
    static int MINi(int a, int b) { return a < b ? a : b; }
    // Mirror GEQ3D's helper so the dims build reads the same across effects: a degenerate (0)
    // grid depth still yields a 1-deep extent. (rectD is a member but does not shadow depth().)
    lengthType depthDim() const { return depth() > 0 ? depth() : 1; }
};

} // namespace mm