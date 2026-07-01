#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

// Masks the layer in a checkerboard pattern: lights in the "off" squares are
// dropped (the physical light maps nowhere), lights in the "on" squares pass
// through unchanged. `size` sets the square edge in lights; `invert` flips which
// squares are on. A mask, not a remap — the logical box is unchanged.
//
// Prior art: MoonLight's Checkerboard modifier (M_MoonLight.h) drops lights by
// setting position to a sentinel; our fold returns false from modifyLogical.
// Author: WildCats08 / @Brandon502 (MoonLight) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class CheckerboardModifier : public ModifierBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin

    uint8_t size = 2;       // checker square edge, in lights (≥1)
    bool invert = false;    // flip which squares pass through

    void onBuildControls() override {
        controls_.addUint8("size", size, 1, 64);
        controls_.addBool("invert", invert);
    }

    // A mask leaves the logical box unchanged (no modifyLogicalSize override).

    bool modifyLogical(Coord3D& pos) const override {
        const lengthType s = size ? size : 1;
        // Parity of the square this light sits in. Even parity = "on" square by
        // default; `invert` swaps which parity passes. Parity is non-negative;
        // compare as signed int so MSVC (/W4) doesn't flag a signed/unsigned mismatch.
        const int parity = ((pos.x / s) + (pos.y / s) + (pos.z / s)) & 1;
        return parity == (invert ? 1 : 0);   // false → dropped (the mask)
    }
};

} // namespace mm
