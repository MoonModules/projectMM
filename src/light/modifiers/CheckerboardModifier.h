#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

// Masks the layer in a checkerboard pattern: lights in the "off" squares are
// dropped (map to no physical position), lights in the "on" squares pass through
// unchanged. `size` sets the square edge in lights; `invert` flips which squares
// are on. The logical box is unchanged (identity dimensions) — this is a mask,
// not a remap.
//
// Implemented by emitting outCount=0 for dropped lights, which Layer::rebuildLUT
// already records as "this logical light has no destination" (the same zero-
// destination path the sparse box→driver translation uses). No ModifierBase
// contract change.
//
// Prior art: MoonLight's Checkerboard modifier (M_MoonLight.h) drops lights by
// setting position.x = UINT16_MAX; outCount=0 is our equivalent.
class CheckerboardModifier : public ModifierBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin

    uint8_t size = 2;       // checker square edge, in lights (≥1)
    bool invert = false;    // flip which squares pass through

    nrOfLightsType maxMultiplier() const override { return 1; }  // 1:1 or 1:0, never fans out

    void onBuildControls() override {
        controls_.addUint8("size", size, 1, 64);
        controls_.addBool("invert", invert);
    }

    // Identity: a mask doesn't resize the logical box.
    void logicalDimensions(lengthType physW, lengthType physH, lengthType physD,
                           lengthType& logW, lengthType& logH, lengthType& logD) const override {
        logW = physW;
        logH = physH;
        logD = physD;
    }

    void mapToPhysical(lengthType lx, lengthType ly, lengthType lz,
                       lengthType physW, lengthType physH, lengthType /*physD*/,
                       nrOfLightsType* outPhysicals, nrOfLightsType& outCount,
                       nrOfLightsType maxOut) const override {
        outCount = 0;
        if (maxOut == 0) return;
        const lengthType s = size ? size : 1;
        // Parity of the square this light sits in. Even parity = "on" square by
        // default; `invert` swaps which parity passes.
        // Parity is non-negative; compare as signed int so MSVC (/W4) doesn't
        // flag a signed/unsigned mismatch against an unsigned literal.
        const int parity = ((lx / s) + (ly / s) + (lz / s)) & 1;
        const bool on = parity == (invert ? 1 : 0);
        if (!on) return;  // dropped — no physical destination (the mask)
        outPhysicals[0] = static_cast<nrOfLightsType>(lz) * static_cast<nrOfLightsType>(physW) * static_cast<nrOfLightsType>(physH) +
                          static_cast<nrOfLightsType>(ly) * static_cast<nrOfLightsType>(physW) +
                          static_cast<nrOfLightsType>(lx);
        outCount = 1;
    }
};

} // namespace mm
