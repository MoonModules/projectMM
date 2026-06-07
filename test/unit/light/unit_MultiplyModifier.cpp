// @module MultiplyModifier

#include "doctest.h"
#include "light/modifiers/MultiplyModifier.h"

// MultiplyModifier tiles the logical image across the physical box `multiply`
// times per axis, optionally reflecting alternate tiles (the kaleidoscope
// mirror). With multiply=2 + mirror on an axis it folds in half — exactly the
// behaviour the old MirrorModifier provided, which several of these cases pin.

// Reports D3 — handles all three axes. Pins the ModifierBase default too.
TEST_CASE("MultiplyModifier advertises D3 dimensions") {
    mm::MultiplyModifier m;
    CHECK(m.dimensions() == mm::Dim::D3);
}

// Defaults (multiply 2/2/1, mirror true/true/false) reproduce the canonical
// mirror-XY pipeline: a 128×128 physical grid → 64×64 logical (each axis folds).
TEST_CASE("MultiplyModifier default logicalDimensions = mirror-XY fold") {
    mm::MultiplyModifier m;
    mm::lengthType logW, logH, logD;
    m.logicalDimensions(128, 128, 1, logW, logH, logD);
    CHECK(logW == 64);   // 128 / 2
    CHECK(logH == 64);
    CHECK(logD == 1);    // multiplyZ default 1 → unchanged
}

// multiplyZ tiles the Z axis too: 128×128×4 with multiply 2/2/2 → 64×64×2.
TEST_CASE("MultiplyModifier logicalDimensions on Z") {
    mm::MultiplyModifier m;
    m.multiplyZ = 2;
    mm::lengthType logW, logH, logD;
    m.logicalDimensions(128, 128, 4, logW, logH, logD);
    CHECK(logW == 64);
    CHECK(logH == 64);
    CHECK(logD == 2);
}

// PURE-FOLD EQUIVALENCE: with the defaults (mult 2, mirror XY), the corner
// logical pixel (0,0) fans out to all four physical corners — byte-identical to
// the old MirrorModifier corner test. This is the canonical-pipeline guarantee.
TEST_CASE("MultiplyModifier corner pixel produces 4 corners (mirror fold)") {
    mm::MultiplyModifier m;  // defaults: mult 2/2/1, mirror true/true/false
    mm::nrOfLightsType physicals[8];
    mm::nrOfLightsType count = 0;

    m.mapToPhysical(0, 0, 0, 128, 128, 1, physicals, count, 8);
    CHECK(count == 4);
    // tile (0,0) identity → (0,0); tile (1,0) mirror x → (127,0);
    // tile (0,1) mirror y → (0,127); tile (1,1) → (127,127). Row-major y*128+x.
    CHECK(physicals[0] == 0);                 // (0,0)
    CHECK(physicals[1] == 127);               // (127,0)
    CHECK(physicals[2] == 127 * 128);         // (0,127)
    CHECK(physicals[3] == 127 * 128 + 127);   // (127,127)
}

// PURE-FOLD EQUIVALENCE: an interior pixel folds to the same two columns the
// old mirrorX-only produced — original + horizontal reflection.
TEST_CASE("MultiplyModifier mirrorX fold matches old Mirror") {
    mm::MultiplyModifier m;
    m.multiplyX = 2; m.mirrorX = true;
    m.multiplyY = 1; m.mirrorY = false;
    m.multiplyZ = 1;
    mm::nrOfLightsType physicals[8];
    mm::nrOfLightsType count = 0;

    m.mapToPhysical(5, 10, 0, 128, 128, 1, physicals, count, 8);
    CHECK(count == 2);
    CHECK(physicals[0] == 10 * 128 + 5);    // (5,10)
    CHECK(physicals[1] == 10 * 128 + 122);  // (127-5,10) = (122,10)
}

// No multiplication on any axis (all multipliers 1) → identity pass-through.
TEST_CASE("MultiplyModifier identity when all multipliers are 1") {
    mm::MultiplyModifier m;
    m.multiplyX = 1; m.multiplyY = 1; m.multiplyZ = 1;
    mm::lengthType logW, logH, logD;
    m.logicalDimensions(128, 128, 1, logW, logH, logD);
    CHECK(logW == 128); CHECK(logH == 128); CHECK(logD == 1);

    mm::nrOfLightsType physicals[8];
    mm::nrOfLightsType count = 0;
    m.mapToPhysical(5, 10, 0, 128, 128, 1, physicals, count, 8);
    CHECK(count == 1);
    CHECK(physicals[0] == 10 * 128 + 5);
}

// Tiling WITHOUT mirror repeats (does not reflect) — multiply 2 on X, mirror off:
// logical x=0 lands at physical x=0 (tile 0) and x=64 (tile 1, identity offset),
// NOT x=127. This is the difference from a fold.
TEST_CASE("MultiplyModifier tiles without mirror (repeat, not fold)") {
    mm::MultiplyModifier m;
    m.multiplyX = 2; m.mirrorX = false;
    m.multiplyY = 1; m.mirrorY = false;
    m.multiplyZ = 1;
    mm::nrOfLightsType physicals[8];
    mm::nrOfLightsType count = 0;

    // 128 wide, tileW = 64. logical x=0 → tile0 x=0, tile1 x=64.
    m.mapToPhysical(0, 0, 0, 128, 128, 1, physicals, count, 8);
    CHECK(count == 2);
    CHECK(physicals[0] == 0);    // tile 0: x=0
    CHECK(physicals[1] == 64);   // tile 1 (no mirror): x = 64+0
}

// multiplyZ on a 2D (depth-1) layout is a no-op: the effective multiplier
// clamps to the axis extent (1), so logD stays 1 and the layer isn't blanked.
// Before the clamp, multiplyZ=4 made logD = 1/4 = 0 → empty layer.
TEST_CASE("MultiplyModifier multiplyZ on 2D does nothing") {
    mm::MultiplyModifier m;
    m.multiplyX = 1; m.multiplyY = 1; m.multiplyZ = 4;  // Z multiply on a flat grid
    mm::lengthType logW, logH, logD;
    m.logicalDimensions(64, 64, 1, logW, logH, logD);
    CHECK(logW == 64);
    CHECK(logH == 64);
    CHECK(logD == 1);   // NOT 0 — Z multiply clamped to the depth-1 extent

    mm::nrOfLightsType physicals[8];
    mm::nrOfLightsType count = 0;
    m.mapToPhysical(5, 10, 0, 64, 64, 1, physicals, count, 8);
    CHECK(count == 1);                       // single position, no Z tiling
    CHECK(physicals[0] == 10 * 64 + 5);      // identity
}

// A multiplier larger than the axis extent clamps to the extent (can't tile more
// times than there are pixels).
TEST_CASE("MultiplyModifier clamps a multiplier above the axis extent") {
    mm::MultiplyModifier m;
    m.multiplyX = 64; m.multiplyY = 1; m.multiplyZ = 1;
    m.mirrorX = false;
    mm::lengthType logW, logH, logD;
    m.logicalDimensions(16, 16, 1, logW, logH, logD);  // 64× on a 16-wide axis
    CHECK(logW == 1);   // clamped to extent 16 → 16/16 = 1, not 16/64 = 0
    CHECK(logH == 16);
}

// maxMultiplier is the product of the raw controls (the fan-out upper bound).
TEST_CASE("MultiplyModifier maxMultiplier is the product of axes") {
    mm::MultiplyModifier m;
    m.multiplyX = 2; m.multiplyY = 2; m.multiplyZ = 2;
    CHECK(m.maxMultiplier() == 8);
    m.multiplyZ = 1;
    CHECK(m.maxMultiplier() == 4);   // the default-ish XY fold
}

// REGRESSION: maxMultiplier() must NOT wrap when all axes are maxed. The product
// 64×64×16 = 65536 overflows nrOfLightsType (uint16 on no-PSRAM) and would wrap
// to 0 — feeding the uint64 maxDest math in Layer::rebuildLUT an already-wrapped
// (possibly 0) multiplier → empty LUT → black display. It must saturate to the
// type max instead. (Single-axis tests above stay under the wrap; this one
// crosses it.) On uint32 (PSRAM) the product fits and isn't saturated — assert
// only the non-wrap, non-zero invariant that holds on both widths.
TEST_CASE("MultiplyModifier maxMultiplier saturates, never wraps to 0") {
    mm::MultiplyModifier m;
    m.multiplyX = 64; m.multiplyY = 64; m.multiplyZ = 16;  // 65536 — wraps uint16
    CHECK(m.maxMultiplier() > 0);                          // never the wrapped 0
    // The product (65536) is ≥ the uint16 ceiling, so on a uint16 build it
    // saturates to 65535; on uint32 it's the true 65536. Either way it's a large
    // positive upper bound, never a small/zero value that would starve the LUT.
    CHECK(m.maxMultiplier() >= 65535);
}

// REGRESSION: an 8×8 multiply must emit all 64 tile positions, not be truncated
// to 8. The Layer's scratch buffer is sized to ModifierBase::kMaxFanout (64); a
// smaller buffer (the original physicals[8]) silently dropped 56 of the 64 tiles,
// so a 128×128 grid showed only 8 tiles instead of the full 8×8 = 64.
TEST_CASE("MultiplyModifier 8x8 emits all 64 tiles") {
    mm::MultiplyModifier m;
    m.multiplyX = 8; m.multiplyY = 8; m.multiplyZ = 1;
    m.mirrorX = false; m.mirrorY = false;  // pure tiling → 64 distinct positions
    CHECK(m.maxMultiplier() == 64);
    mm::nrOfLightsType physicals[64];
    mm::nrOfLightsType count = 0;
    // 128 wide → tile edge 16; logical (0,0) maps to one position per 16×16 tile.
    m.mapToPhysical(0, 0, 0, 128, 128, 1, physicals, count, 64);
    CHECK(count == 64);
}

// Fan-out never exceeds maxOut even if asked for more than the buffer holds.
TEST_CASE("MultiplyModifier respects maxOut clamp") {
    mm::MultiplyModifier m;
    m.multiplyX = 2; m.multiplyY = 2; m.multiplyZ = 2;  // wants 8
    mm::nrOfLightsType physicals[8];
    mm::nrOfLightsType count = 0;
    m.mapToPhysical(0, 0, 0, 128, 128, 8, physicals, count, 4);  // cap at 4
    CHECK(count == 4);
}
