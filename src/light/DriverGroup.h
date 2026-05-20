#pragma once

#include "core/MoonModule.h"
#include "light/Buffer.h"
#include "light/Layer.h"
#include "light/BlendMap.h"
#include "platform/platform.h"

namespace mm {

class DriverBase : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Driver; }
    virtual void setSourceBuffer(Buffer* buf) = 0;
};

class DriverGroup : public MoonModule {
public:
    void setLayer(Layer* layer) {
        layer_ = layer;
    }

    void setup() override {
        MoonModule::setup();
        passBufferToDrivers();
    }

    void onAllocateMemory() override {
        if (layer_ && !layer_->lut().isOneToOne()) {
            outputBuffer_.allocate(layer_->physicalLightCount(), layer_->channelsPerLight());
        }
        passBufferToDrivers();
        MoonModule::onAllocateMemory();
    }

    void loop() override {
        if (layer_ && !layer_->lut().isOneToOne()) {
            blendMap(layer_->buffer(), outputBuffer_, layer_->lut(), layer_->channelsPerLight());
        }
        for (uint8_t i = 0; i < childCount(); i++) {
            uint32_t start = platform::micros();
            child(i)->loop();
            child(i)->addAccumUs(platform::micros() - start);
        }
    }

private:
    Layer* layer_ = nullptr;
    Buffer outputBuffer_;

    void passBufferToDrivers() {
        if (!layer_) return;
        Buffer* buf = layer_->lut().isOneToOne() ? &layer_->buffer() : &outputBuffer_;
        for (uint8_t i = 0; i < childCount(); i++) {
            static_cast<DriverBase*>(child(i))->setSourceBuffer(buf);
        }
    }
};

} // namespace mm
