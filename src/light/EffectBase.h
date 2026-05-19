#pragma once

#include "core/MoonModule.h"
#include "core/types.h"

#include <cstdint>

namespace mm {

class Layer; // forward declaration

class EffectBase : public MoonModule {
public:
    void setLayer(Layer* l) { layer_ = l; }
    Layer* layer() const { return layer_; }

    // Convenience accessors — delegate to parent Layer
    // Defined after Layer is complete (in Layer.h)
    uint8_t* buffer();
    lengthType width() const;
    lengthType height() const;
    lengthType depth() const;
    uint8_t channelsPerLight() const;
    nrOfLightsType nrOfLights() const;
    uint32_t elapsed() const;

private:
    Layer* layer_ = nullptr;
};

} // namespace mm
