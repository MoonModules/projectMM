#pragma once

#include "core/MoonModule.h"
#include "light/Buffer.h"
#include "light/Layer.h"

#include <array>

namespace mm {

class DriverBase : public MoonModule {
public:
    virtual void setSourceBuffer(Buffer* buf) = 0;
};

class DriverGroup : public MoonModule {
public:
    void addDriver(DriverBase* driver) {
        if (!driver || driverCount_ >= drivers_.size()) return;
        driver->setParent(this);
        drivers_[driverCount_++] = driver;
    }

    void setLayer(Layer* layer) {
        layer_ = layer;
    }

    void setup() override {
        for (uint8_t i = 0; i < driverCount_; i++) {
            drivers_[i]->setup();
        }
        // Pass the layer buffer to all drivers
        if (layer_) {
            for (uint8_t i = 0; i < driverCount_; i++) {
                drivers_[i]->setSourceBuffer(&layer_->buffer());
            }
        }
    }

    void onBuildControls() override {
        for (uint8_t i = 0; i < driverCount_; i++) {
            drivers_[i]->onBuildControls();
        }
    }

    void onAllocateMemory() override {
        for (uint8_t i = 0; i < driverCount_; i++) {
            drivers_[i]->onAllocateMemory();
        }
        // Re-pass buffer after layer reallocates
        if (layer_) {
            for (uint8_t i = 0; i < driverCount_; i++) {
                drivers_[i]->setSourceBuffer(&layer_->buffer());
            }
        }
    }

    void loop() override {
        for (uint8_t i = 0; i < driverCount_; i++) {
            drivers_[i]->loop();
        }
    }

    void teardown() override {
        for (uint8_t i = driverCount_; i > 0; i--) {
            drivers_[i - 1]->teardown();
        }
    }

private:
    std::array<DriverBase*, 4> drivers_{};
    uint8_t driverCount_ = 0;
    Layer* layer_ = nullptr;
};

} // namespace mm
