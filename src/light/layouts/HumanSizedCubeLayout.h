#pragma once

#include <limits>
#include <cstdint>
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType

namespace mm {

// A human-sized LED cube built from five hanging "curtain" faces of a
// width×height×depth box: front (z=0), back (z=depth+1), above (y=0),
// left (x=0) and right (x=width+1). Each face is a flat matrix of lights;
// the +1 offsets inset the four side/top curtains by one cell so they hang
// just outside the front/back planes (front sits at z=0, back at z=depth+1,
// with the wrap-around curtains bridging them). The bottom face is left open
// (a person stands inside), so only five of the six faces are emitted.
//
// Prior art: MoonLight's HumanSizedCubeLayout (Node "Human Sized Cube",
// tags 🚥). The geometry is reproduced exactly: the five face loops, their
// nested axis order, and every +1 / depth+1 / width+1 offset match the
// source's onLayout() addLight() sequence, so the emitted coordinates and
// their wiring order are identical. MoonLight's per-curtain nextPin() calls
// and the commented-out "below" (y=height+1) face are dropped — a projectMM
// layout emits coordinates only (the driver owns pins), and the sixth face
// is disabled in the source. tags 💫 marks the MoonLight lineage.
// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
class HumanSizedCubeLayout : public LayoutBase {
public:
    // Verbatim MoonLight defaults and ranges: a 10×10×10 cube, each edge 1..20.
    uint8_t width  = 10;
    uint8_t height = 10;
    uint8_t depth  = 10;

    void onBuildControls() override {
        controls_.addUint8("width",  width,  1, 20);
        controls_.addUint8("height", height, 1, 20);
        controls_.addUint8("depth",  depth,  1, 20);
    }

    const char* tags() const override { return "💫"; }

    nrOfLightsType lightCount() const override {
        // Sum of the five face areas, matching the loop bounds in forEachCoord:
        //   front + back : width*height each   (2 * w*h)
        //   above        : width*depth         (w*d)
        //   left + right : depth*height each   (2 * d*h)
        // Computed in uint32_t to detect overflow before the clamp, per GridLayout.
        const uint32_t w = width, h = height, d = depth;
        uint32_t n = 2u * w * h + w * d + 2u * d * h;
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        const uint32_t limit = lightCount();
        uint32_t idx = 0;

        // Each face reproduces one MoonLight loop verbatim, in the same order the
        // source's onLayout() calls addLight(), so driver index i lands on the same
        // physical curtain cell it does in MoonLight.

        // front: z = 0  — for x { for y }
        for (lengthType x = 0; x < width && idx < limit; x++)
            for (lengthType y = 0; y < height && idx < limit; y++)
                cb(ctx, static_cast<nrOfLightsType>(idx++),
                   static_cast<lengthType>(x + 1), static_cast<lengthType>(y + 1), 0);

        // back: z = depth+1  — for x { for y }
        for (lengthType x = 0; x < width && idx < limit; x++)
            for (lengthType y = 0; y < height && idx < limit; y++)
                cb(ctx, static_cast<nrOfLightsType>(idx++),
                   static_cast<lengthType>(x + 1), static_cast<lengthType>(y + 1),
                   static_cast<lengthType>(depth + 1));

        // above: y = 0  — for x { for z }
        for (lengthType x = 0; x < width && idx < limit; x++)
            for (lengthType z = 0; z < depth && idx < limit; z++)
                cb(ctx, static_cast<nrOfLightsType>(idx++),
                   static_cast<lengthType>(x + 1), 0, static_cast<lengthType>(z + 1));

        // below (y = height+1) is intentionally omitted — commented out in the
        // MoonLight source (the cube is open at the bottom).

        // left: x = 0  — for z { for y }
        for (lengthType z = 0; z < depth && idx < limit; z++)
            for (lengthType y = 0; y < height && idx < limit; y++)
                cb(ctx, static_cast<nrOfLightsType>(idx++),
                   0, static_cast<lengthType>(y + 1), static_cast<lengthType>(z + 1));

        // right: x = width+1  — for z { for y }
        for (lengthType z = 0; z < depth && idx < limit; z++)
            for (lengthType y = 0; y < height && idx < limit; y++)
                cb(ctx, static_cast<nrOfLightsType>(idx++),
                   static_cast<lengthType>(width + 1), static_cast<lengthType>(y + 1),
                   static_cast<lengthType>(z + 1));
    }
};

} // namespace mm
