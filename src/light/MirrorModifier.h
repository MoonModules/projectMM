#pragma once

#include "light/ModifierBase.h"

namespace mm {

class MirrorModifier : public ModifierBase {
public:
    const char* tags() const override { return "🪞"; }

    bool mirrorX = true;
    bool mirrorY = true;
    bool mirrorZ = false;

    uint8_t maxMultiplier() const override {
        return (mirrorX ? 2 : 1) * (mirrorY ? 2 : 1) * (mirrorZ ? 2 : 1);
    }

    void onBuildControls() override {
        controls_.addBool("mirrorX", mirrorX);
        controls_.addBool("mirrorY", mirrorY);
        controls_.addBool("mirrorZ", mirrorZ);
    }

    void logicalDimensions(lengthType physW, lengthType physH, lengthType physD,
                           lengthType& logW, lengthType& logH, lengthType& logD) const override {
        logW = mirrorX ? (physW + 1) / 2 : physW;
        logH = mirrorY ? (physH + 1) / 2 : physH;
        logD = mirrorZ ? (physD + 1) / 2 : physD;
    }

    void mapToPhysical(lengthType lx, lengthType ly, lengthType lz,
                       lengthType physW, lengthType physH, lengthType physD,
                       nrOfLightsType* outPhysicals, nrOfLightsType& outCount,
                       nrOfLightsType maxOut) const override {
        outCount = 0;

        // Generate mirror coordinates for each axis
        lengthType xs[2] = {lx, static_cast<lengthType>(physW - 1 - lx)};
        lengthType ys[2] = {ly, static_cast<lengthType>(physH - 1 - ly)};
        lengthType zs[2] = {lz, static_cast<lengthType>(physD - 1 - lz)};

        int xCount = (mirrorX && xs[1] != xs[0]) ? 2 : 1;
        int yCount = (mirrorY && ys[1] != ys[0]) ? 2 : 1;
        int zCount = (mirrorZ && zs[1] != zs[0]) ? 2 : 1;

        for (int zi = 0; zi < zCount && outCount < maxOut; zi++) {
            for (int yi = 0; yi < yCount && outCount < maxOut; yi++) {
                for (int xi = 0; xi < xCount && outCount < maxOut; xi++) {
                    nrOfLightsType idx = static_cast<nrOfLightsType>(zs[zi]) *
                                         static_cast<nrOfLightsType>(physW) *
                                         static_cast<nrOfLightsType>(physH) +
                                         static_cast<nrOfLightsType>(ys[yi]) *
                                         static_cast<nrOfLightsType>(physW) +
                                         static_cast<nrOfLightsType>(xs[xi]);
                    outPhysicals[outCount++] = idx;
                }
            }
        }
    }
};

} // namespace mm
