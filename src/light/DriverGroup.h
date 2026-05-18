#pragma once

#include "core/MoonModule.h"
#include "light/BlendMap.h"
#include "light/Buffer.h"
#include "light/DriverBase.h"
#include "light/Layer.h"
#include <cstdint>

namespace mm::light {

// Groups drivers. Owns the shared output buffer.
// In loop(): calls blendMap from layers, then loop() on all drivers.
class DriverGroup : public MoonModule {
public:
    const char* name() const override { return "DriverGroup"; }

    static constexpr uint8_t MAX_DRIVERS = 8;

    void addDriver(DriverBase* driver) {
        if (driverCount_ < MAX_DRIVERS) {
            drivers_[driverCount_++] = driver;
        }
    }

    void setLayers(const Layer* layers, size_t count) {
        layers_ = layers;
        layerCount_ = count;
    }

    // Allocate/reallocate output buffer to match layout size.
    // Call when layouts or layers change (cold path).
    void allocateOutput(size_t pixelCount) {
        outputBuffer_.allocate(pixelCount);
    }

    void loop() override {
        // Blend+map all layers into the shared output buffer
        if (layers_ && layerCount_ > 0 && outputBuffer_.count() > 0) {
            blendMap(outputBuffer_.pixels(), layers_, layerCount_);
        }

        // Pass output buffer to each driver and call loop()
        auto outSpan = std::span<const RGB>(outputBuffer_.pixels().data(),
                                             outputBuffer_.pixels().size());
        for (uint8_t i = 0; i < driverCount_; ++i) {
            drivers_[i]->setOutputBuffer(outSpan);
            drivers_[i]->loop();
        }
    }

    uint8_t driverCount() const { return driverCount_; }
    const Buffer& outputBuffer() const { return outputBuffer_; }

private:
    DriverBase* drivers_[MAX_DRIVERS] = {};
    uint8_t driverCount_ = 0;
    const Layer* layers_ = nullptr;
    size_t layerCount_ = 0;
    Buffer outputBuffer_;
};

} // namespace mm::light
