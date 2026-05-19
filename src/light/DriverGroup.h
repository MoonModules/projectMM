#pragma once

#include "core/MoonModule.h"
#include "light/Buffer.h"
#include "light/Layer.h"
#include "light/BlendMap.h"

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
        passBufferToDrivers();
    }

    void onBuildControls() override {
        for (uint8_t i = 0; i < driverCount_; i++) {
            drivers_[i]->onBuildControls();
        }
    }

    void onAllocateMemory() override {
        if (layer_ && !layer_->lut().isOneToOne()) {
            outputBuffer_.allocate(layer_->physicalLightCount(), layer_->channelsPerLight());
        }
        passBufferToDrivers();
        for (uint8_t i = 0; i < driverCount_; i++) {
            drivers_[i]->onAllocateMemory();
        }
    }

    void loop() override {
        if (layer_ && !layer_->lut().isOneToOne()) {
            // Blend+map: logical buffer → physical output buffer via LUT
            blendMap(layer_->buffer(), outputBuffer_, layer_->lut(), layer_->channelsPerLight());
        }
        for (uint8_t i = 0; i < driverCount_; i++) {
            drivers_[i]->loop();
        }
    }

    void teardown() override {
        for (uint8_t i = driverCount_; i > 0; i--) {
            drivers_[i - 1]->teardown();
        }
    }

    uint8_t driverCount() const { return driverCount_; }
    DriverBase* driver(uint8_t i) const { return i < driverCount_ ? drivers_[i] : nullptr; }

    uint8_t childCount() const override { return driverCount_; }
    MoonModule* child(uint8_t i) const override { return driver(i); }

private:
    std::array<DriverBase*, 4> drivers_{};
    uint8_t driverCount_ = 0;
    Layer* layer_ = nullptr;
    Buffer outputBuffer_;

    void passBufferToDrivers() {
        if (!layer_) return;
        Buffer* buf = layer_->lut().isOneToOne() ? &layer_->buffer() : &outputBuffer_;
        for (uint8_t i = 0; i < driverCount_; i++) {
            drivers_[i]->setSourceBuffer(buf);
        }
    }
};

} // namespace mm
