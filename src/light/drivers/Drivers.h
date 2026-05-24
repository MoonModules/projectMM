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
    // Optional: drivers that need dimensions (e.g. PreviewDriver describing the LED grid in
    // the WebSocket frame) call layer_ for current physical width/height/depth. ArtNet doesn't
    // need it — it just streams bytes.
    //
    // Multi-layer: this is the *active* layer for dimension queries, not a 1:1 wiring
    // constraint. When the multi-layer pipeline arrives, Drivers composes/blends N
    // layer buffers upstream and still hands each driver one Layer for dimensions —
    // the driver outputs to a single physical fixture either way. See plan.md backlog.
    void setLayer(Layer* layer) { layer_ = layer; }
protected:
    Layer* layer_ = nullptr;
};

class Drivers : public MoonModule {
public:
    void setLayer(Layer* layer) {
        layer_ = layer;
    }

    void setup() override {
        MoonModule::setup();
        passBufferToDrivers();
    }

    void onAllocateMemory() override {
        // Output buffer needed if any layer has a LUT (currently single layer).
        // Multi-layer: check all layers, allocate if at least one has a LUT.
        if (layer_ && layer_->lut().hasLUT()) {
            outputBuffer_.allocate(layer_->physicalLightCount(), layer_->channelsPerLight());
        } else {
            outputBuffer_.free();
        }
        setDynamicBytes(outputBuffer_.bytes());
        passBufferToDrivers();
        MoonModule::onAllocateMemory();
    }

    void loop() override {
        // Scheduler gates Drivers itself via respectsEnabled() default. The Scheduler
        // only walks the top-level module list, so it never sees the driver children
        // here — we have to honour each child's `enabled` flag ourselves, same as
        // Layer::loop() does for effects and Layers::loop() does for child Layers.
        // Without this gate the UI's enable/disable on an ArtNet or Preview driver
        // is a no-op and the driver keeps emitting.
        if (layer_ && layer_->lut().hasLUT()) {
            blendMap(layer_->buffer(), outputBuffer_, layer_->lut(), layer_->channelsPerLight());
        }
        for (uint8_t i = 0; i < childCount(); i++) {
            if (!child(i)->enabled()) continue;
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
        Buffer* buf = layer_->lut().hasLUT() ? &outputBuffer_ : &layer_->buffer();
        for (uint8_t i = 0; i < childCount(); i++) {
            auto* drv = static_cast<DriverBase*>(child(i));
            drv->setSourceBuffer(buf);
            drv->setLayer(layer_);  // so PreviewDriver can read current physical dimensions
        }
    }
};

} // namespace mm
