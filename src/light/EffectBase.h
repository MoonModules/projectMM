#pragma once

#include "core/MoonModule.h"
#include "core/types.h"

#include <cstdint>

namespace mm {

class Layer; // forward declaration

class EffectBase : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Effect; }

    // Parent is always a Layer (defined in Layer.h after Layer is complete)
    Layer* layer() const;

    // Convenience accessors — delegate to parent Layer
    // Defined after Layer is complete (in Layer.h)
    uint8_t* buffer();
    lengthType width() const;
    lengthType height() const;
    lengthType depth() const;
    uint8_t channelsPerLight() const;
    nrOfLightsType nrOfLights() const;
    uint32_t elapsed() const;
};

} // namespace mm
