#pragma once

#include "core/MoonModule.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layers/BlendMap.h"
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
        // If allocation fails (no contiguous heap large enough — a real risk on
        // ESP32 without PSRAM when the Layer pipeline has fragmented DRAM),
        // outputBuffer_ stays with data_=nullptr. loop() must check that before
        // calling blendMap, otherwise blendMap will dereference a null
        // outputBuffer_.data() and panic with LoadProhibited. Same defensive
        // pattern Layer::allocateBuffer uses for its pixel buffer.
        if (layer_ && layer_->lut().hasLUT()) {
            if (!outputBuffer_.allocate(layer_->physicalLightCount(), layer_->channelsPerLight())) {
                std::printf("  DEGRADE  Drivers::outputBuffer_ allocate failed for %u lights\n",
                            static_cast<unsigned>(layer_->physicalLightCount()));
                outputBuffer_.free();   // leaves data_=nullptr, bytes()=0
            }
        } else {
            outputBuffer_.free();
        }
        setDynamicBytes(outputBuffer_.bytes());
        passBufferToDrivers();
        MoonModule::onAllocateMemory();
    }

    void loop() override {
        // outputBuffer_.data() can be null if onAllocateMemory failed to claim
        // a contiguous block (heap fragmentation). Skip the blend in that case
        // — drivers run on raw Layer buffer or simply have nothing to send.
        if (layer_ && layer_->lut().hasLUT() && outputBuffer_.data()) {
            blendMap(layer_->buffer(), outputBuffer_, layer_->lut(), layer_->channelsPerLight());
        }
        // Option A: parent work first (blendMap), then chain to base to tick
        // children on the freshly-blended buffer. Per-child enabled gating and
        // timing accumulation live in MoonModule::tickChildren.
        MoonModule::loop();
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
