#pragma once

#include "core/MoonModule.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layers/BlendMap.h"
#include "light/drivers/Correction.h"
#include "platform/platform.h"

#include <cstring>  // std::strcmp in onUpdate

namespace mm {

class DriverBase : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Driver; }
    virtual void setSourceBuffer(Buffer* buf) = 0;
    // Optional: drivers that need dimensions (e.g. PreviewDriver describing the LED grid in
    // the WebSocket frame) call layer_ for current physical width/height/depth. ArtNet doesn't
    // need it — it just streams bytes.
    //
    // This is the *active* layer for dimension queries, not a 1:1 wiring
    // constraint: each driver outputs to a single physical fixture, and the
    // Drivers container hands it one Layer for dimensions regardless of how many
    // layers feed the output buffer.
    void setLayer(Layer* layer) { layer_ = layer; }

    // Shared output correction (brightness LUT + channel order + white) owned by the
    // Drivers container. Default no-op so Preview (which shows the raw logical buffer)
    // and any future preview-style driver opt out for free; only physical drivers
    // (ArtNet, future LED) override to apply it.
    virtual void setCorrection(const Correction* /*c*/) {}

    // Notified by Drivers when the shared Correction's outChannels may have changed
    // (a lightPreset switch RGB↔RGBW). Default no-op; physical drivers that own an
    // intermediate correction-applied buffer override to resize it OFF the hot path.
    // Topology changes (light count, channels per light) already flow through
    // onBuildState — this hook is just for the preset-driven channel-count change
    // that doesn't trigger a structural rebuild.
    virtual void onCorrectionChanged() {}
protected:
    Layer* layer_ = nullptr;
};

class Drivers : public MoonModule {
public:
    const char* acceptsChildRoles() const override { return "driver"; }

    uint8_t brightness = 255;
    uint8_t lightPreset = 0;  // index into kLightPresetOptions; 0 = RGB

    void setLayer(Layer* layer) {
        layer_ = layer;
    }

    void onBuildControls() override {
        controls_.addUint8("brightness", brightness, 0, 255);
        controls_.addSelect("lightPreset", lightPreset, kLightPresetOptions, kLightPresetCount);
        MoonModule::onBuildControls();  // cascade to driver children
    }

    // Brightness / light-preset changes only rebuild the (cheap) correction LUT — no
    // pipeline realloc. This is what keeps the brightness slider fluent: controlChangeTriggersBuildState
    // stays false for Drivers, so handleSetControl skips scheduler_->buildState().
    void onUpdate(const char* controlName) override {
        if (std::strcmp(controlName, "brightness") == 0 ||
            std::strcmp(controlName, "lightPreset") == 0) {
            correction_.rebuild(brightness, static_cast<LightPreset>(lightPreset));
            // Propagate so physical drivers that maintain a correction-applied
            // buffer (today: ArtNet) can resize off the hot path. A brightness-
            // only change is a no-op for resizing (outChannels stays 3); the
            // RGB↔RGBW preset switch is the case that actually grows/shrinks.
            for (uint8_t i = 0; i < childCount(); i++) {
                static_cast<DriverBase*>(child(i))->onCorrectionChanged();
            }
        }
    }

    void setup() override {
        correction_.rebuild(brightness, static_cast<LightPreset>(lightPreset));
        MoonModule::setup();
        passBufferToDrivers();
    }

    void onBuildState() override {
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
        MoonModule::onBuildState();
    }

    void loop() override {
        // outputBuffer_.data() can be null if onBuildState failed to claim
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
    Correction correction_;

    void passBufferToDrivers() {
        if (!layer_) return;
        Buffer* buf = layer_->lut().hasLUT() ? &outputBuffer_ : &layer_->buffer();
        for (uint8_t i = 0; i < childCount(); i++) {
            auto* drv = static_cast<DriverBase*>(child(i));
            drv->setSourceBuffer(buf);
            drv->setLayer(layer_);  // so PreviewDriver can read current physical dimensions
            drv->setCorrection(&correction_);  // physical drivers apply it; Preview ignores
        }
    }
};

} // namespace mm
