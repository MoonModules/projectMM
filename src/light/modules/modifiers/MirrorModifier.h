#pragma once

#include "core/MoonModule.h"
#include <cstdint>

namespace mm::light {

// Static modifier: mirrors the logical space around the centre axes.
// With mirrorX+Y: logical buffer is 1/4 of physical, each logical pixel
// maps to 4 physical positions (1:4). With all three: 1:8.
// Zero cost at render time — the mapping is baked into the LUT.
class MirrorModifier : public MoonModule {
public:
    const char* name() const override { return "Mirror"; }

    void addControls() override {
        mirrorXIdx_ = MoonModule::addControl("mirrorX", true);
        mirrorYIdx_ = MoonModule::addControl("mirrorY", true);
        mirrorZIdx_ = MoonModule::addControl("mirrorZ", true);
    }

    void onChange(uint8_t) override { markDirty(); }

    bool mirrorX() const { return control(mirrorXIdx_)->b.value; }
    bool mirrorY() const { return control(mirrorYIdx_)->b.value; }
    bool mirrorZ() const { return control(mirrorZIdx_)->b.value; }

    // How many times each logical pixel is replicated
    uint8_t multiplier() const {
        uint8_t m = 1;
        if (mirrorX()) m *= 2;
        if (mirrorY()) m *= 2;
        if (mirrorZ()) m *= 2;
        return m;
    }

    // Logical dimensions (reduced by mirror)
    int16_t logicalWidth(int16_t w) const { return mirrorX() ? (w + 1) / 2 : w; }
    int16_t logicalHeight(int16_t h) const { return mirrorY() ? (h + 1) / 2 : h; }
    int16_t logicalDepth(int16_t d) const { return mirrorZ() ? (d + 1) / 2 : d; }

    // Given a logical coordinate (in reduced space), produce all physical
    // positions it maps to. Returns the count written to 'out'.
    // out must have room for multiplier() entries.
    uint8_t mapToPhysical(int16_t lx, int16_t ly, int16_t lz,
                          int16_t w, int16_t h, int16_t d,
                          uint16_t* out, int16_t physW) const {
        // Collect mirrored coordinates per axis
        int16_t xs[2], ys[2], zs[2];
        uint8_t nx = 1, ny = 1, nz = 1;
        xs[0] = lx;
        ys[0] = ly;
        zs[0] = lz;
        if (mirrorX() && w > 0) { xs[1] = w - 1 - lx; nx = 2; }
        if (mirrorY() && h > 0) { ys[1] = h - 1 - ly; ny = 2; }
        if (mirrorZ() && d > 0) { zs[1] = d - 1 - lz; nz = 2; }

        uint8_t count = 0;
        for (uint8_t zi = 0; zi < nz; ++zi) {
            for (uint8_t yi = 0; yi < ny; ++yi) {
                for (uint8_t xi = 0; xi < nx; ++xi) {
                    int16_t px = xs[xi];
                    int16_t py = ys[yi];
                    int16_t pz = zs[zi];
                    // Skip duplicates (when coord is exactly on centre axis)
                    uint16_t physIdx = static_cast<uint16_t>(
                        px + py * physW + pz * physW * h);
                    // Check for duplicate
                    bool dup = false;
                    for (uint8_t k = 0; k < count; ++k) {
                        if (out[k] == physIdx) { dup = true; break; }
                    }
                    if (!dup) out[count++] = physIdx;
                }
            }
        }
        return count;
    }

private:
    uint8_t mirrorXIdx_ = 0;
    uint8_t mirrorYIdx_ = 0;
    uint8_t mirrorZIdx_ = 0;
};

} // namespace mm::light
