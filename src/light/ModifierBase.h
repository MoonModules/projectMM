#pragma once

#include "core/MoonModule.h"
#include "core/types.h"

namespace mm {

class ModifierBase : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Modifier; }
    virtual bool isStatic() const { return true; }

    // Max physical destinations per logical light (for LUT allocation estimate)
    virtual uint8_t maxMultiplier() const { return 8; }

    // Compute logical dimensions given physical dimensions
    virtual void logicalDimensions(lengthType physW, lengthType physH, lengthType physD,
                                   lengthType& logW, lengthType& logH, lengthType& logD) const = 0;

    // Map a logical coordinate to physical positions
    // outPhysicals: caller-provided array (stack, max 8 for XYZ mirror)
    // outCount: set to number of physical positions written
    virtual void mapToPhysical(lengthType lx, lengthType ly, lengthType lz,
                               lengthType physW, lengthType physH, lengthType physD,
                               nrOfLightsType* outPhysicals, nrOfLightsType& outCount,
                               nrOfLightsType maxOut) const = 0;
};

} // namespace mm
