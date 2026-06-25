#pragma once

#include "core/MoonModule.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layers/Layers.h"
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
    // The active Layer this driver reads dimensions from — null when no Layer is
    // wired (e.g. the last Layer was deleted). Drivers must tolerate null here.
    Layer* layer() const { return layer_; }

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

    // Clear both shared status strings on teardown (frees the owned failBuf_). A
    // driver that overrides teardown() for its own peripheral cleanup chains to
    // this afterwards: `deinit(); DriverBase::teardown();`.
    void teardown() override { clearFailBuf(); clearConfigErr(); }

protected:
    Layer* layer_ = nullptr;

    // --- Shared status-string lifecycle for the physical LED drivers (RMT / LCD /
    // Parlio). They report two kinds of transient status that must clear cleanly
    // without stomping an unrelated status set by something else:
    //   configErr_ — a borrowed string literal (a parse-error message);
    //   failBuf_   — an owned, on-demand char buffer (a formatted loopback/init
    //                failure with numbers in it).
    // Both follow the same "clear only MY status" rule: only call clearStatus() if
    // the status currently shown is the one this driver set. This was triplicated
    // verbatim across the three drivers; it lives here once (the No-duplication
    // rule). Preview-style drivers never touch these, so the cost is a couple of
    // null pointers they ignore.
    const char* configErr_ = nullptr;
    char* failBuf_ = nullptr;
    static constexpr size_t kFailBufLen = 48;

    // Record a parse/config error: set the status and remember it so clearConfigErr
    // can later retract exactly this one.
    void setConfigErr(const char* err) {
        configErr_ = err;
        setStatus(err, Severity::Error);
    }
    void clearConfigErr() {
        if (configErr_) {
            if (status() == configErr_) clearStatus();
            configErr_ = nullptr;
        }
    }

    // Lazily allocate the owned fail-message buffer (caller snprintf's into it then
    // setStatus(failBuf_)). Returns null if the allocation fails, in which case the
    // caller falls back to a literal status.
    char* failBufEnsure() {
        if (!failBuf_) failBuf_ = static_cast<char*>(platform::alloc(kFailBufLen));
        return failBuf_;
    }
    void clearFailBuf() {
        if (failBuf_) {
            if (status() == failBuf_) clearStatus();
            platform::free(failBuf_);
            failBuf_ = nullptr;
        }
    }
};

class Drivers : public MoonModule {
public:
    const char* acceptsChildRoles() const override { return "driver"; }

    // Default low (≈8%). A fresh device with LEDs wired but no power budget set
    // (e.g. a strip on USB 5V) draws far less at 20 than at full white, so the
    // first boot can't brown out the board before the user sets a safe level.
    // The user raises it via the brightness control once their supply is known.
    uint8_t brightness = 20;
    // GRB (index 2): the wire order of WS2812/SK6812 strips — the common case,
    // so a freshly-flashed board with a strip attached shows correct colours
    // out of the box. Only the physical output drivers apply this reorder;
    // PreviewDriver reads the RGB source buffer directly, so the simulator is
    // unaffected. RGB-ordered outputs (some ArtNet/network sinks) flip it back.
    uint8_t lightPreset = 2;  // index into kLightPresetOptions; 2 = GRB

    // Two ways to wire the source Layer:
    //  - setLayers(Layers*): bind the container; layer_ is re-resolved from
    //    activeLayer() at every buildState. This makes the link self-healing —
    //    a Layer cleared and rebuilt via the API (clear_children + add_module)
    //    is picked up on the next buildState without re-running main.cpp wiring.
    //  - setLayer(Layer*): pin a specific Layer directly (test rigs that build a
    //    Layer outside a Layers container). Skips re-resolution.
    void setLayers(Layers* layers) {
        layers_ = layers;
        if (layers_) layer_ = layers_->activeLayer();
    }
    void setLayer(Layer* layer) {
        layers_ = nullptr;  // explicit pin overrides container resolution
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
        // Re-resolve the active Layer from the bound container so a Layer that
        // was cleared and rebuilt via the API is picked up here (self-healing).
        // setLayer() pins a Layer directly and leaves layers_ null — skip then.
        if (layers_) layer_ = layers_->activeLayer();
        // The output (composition) buffer is needed when we must blend into a
        // physical-space buffer rather than hand a driver a Layer's logical buffer
        // directly: whenever ≥2 layers composite, OR a single layer has a LUT
        // (logical≠physical). A lone no-LUT layer needs no output buffer (drivers
        // read its buffer directly — the zero-copy fast path).
        // If allocation fails (no contiguous heap — a real risk on no-PSRAM ESP32
        // with fragmented DRAM), outputBuffer_ stays data_=nullptr; loop() checks
        // that before blending (else a null deref panics — same defensive pattern
        // Layer::allocateBuffer uses). Sized from the active layer: every layer
        // composites into the same physical space, so its physicalLightCount() /
        // channelsPerLight() is the composite extent.
        // Output selection keys off an *enabled* source layer, never the disabled
        // fallback activeLayer() may return (which exists only so geometry stays
        // queryable while every layer is toggled off). With no enabled layer there
        // is nothing to emit, so no output buffer — drivers go idle (see
        // passBufferToDrivers). A pinned setLayer() (layers_ null) is always treated
        // as the live source.
        Layer* const out = layers_ ? layers_->firstEnabledLayer() : layer_;
        const uint8_t enabled = layers_ ? layers_->enabledLayerCount() : (layer_ ? 1 : 0);
        const bool needOutput = out && (enabled > 1 || out->lut().hasLUT());
        if (needOutput) {
            if (!outputBuffer_.allocate(out->physicalLightCount(), out->channelsPerLight())) {
                std::printf("  DEGRADE  Drivers::outputBuffer_ allocate failed for %u lights\n",
                            static_cast<unsigned>(out->physicalLightCount()));
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
        // Composite into outputBuffer_ when one is allocated (≥2 enabled layers,
        // or a single layer with a LUT — see onBuildState). A null data_ means
        // onBuildState couldn't claim a block (heap fragmentation): skip the blend;
        // drivers then read the raw Layer buffer / send nothing.
        if (outputBuffer_.data() && layers_ && layers_->enabledLayerCount() > 1) {
            // Multi-layer composite: blend each enabled layer in container order.
            // The first (bottom) layer clears + overwrites; each subsequent layer
            // blends onto the accumulated frame per its own blendMode + opacity.
            // blendMap resolves the op/opacity branch once per layer (a tight
            // specialized loop each — no-LUT layers blend 1:1, LUT layers map),
            // and a full-opacity additive/overwrite layer pays no alpha math, so
            // cost scales with enabled-layer count only.
            layers_->forEachEnabledLayer([&](Layer* L, bool first) {
                BlendOp op = first ? BlendOp::Overwrite : L->blendOp();
                uint8_t op_opacity = first ? 255 : L->opacity;
                blendMap(L->buffer(), outputBuffer_, L->lut(), L->channelsPerLight(),
                         op, op_opacity, /*clearFirst=*/first);
            });
        } else if (Layer* out = layers_ ? layers_->firstEnabledLayer() : layer_;
                   outputBuffer_.data() && out && out->lut().hasLUT()) {
            // Single layer with a LUT (the only enabled one, or a pinned setLayer):
            // map its logical buffer into physical space. The original fast path.
            // `out` is the enabled source, never activeLayer()'s disabled fallback;
            // the outputBuffer_.data() guard already excludes the all-disabled case
            // (needOutput is false then), this keeps the source choice explicit.
            blendMap(out->buffer(), outputBuffer_, out->lut(), out->channelsPerLight());
        }
        // (A lone enabled no-LUT layer skips the above — drivers read its logical
        // buffer directly, the zero-copy path set in passBufferToDrivers.)
        //
        // Option A: parent work first (blend), then chain to base to tick children
        // on the freshly-composited buffer. Per-child enabled gating + timing live
        // in MoonModule::tickChildren.
        MoonModule::loop();
    }

private:
    Layers* layers_ = nullptr;  // bound container; layer_ re-resolved from it at buildState
    Layer* layer_ = nullptr;
    Buffer outputBuffer_;
    Correction correction_;

    void passBufferToDrivers() {
        // No active Layer (e.g. the last Layer was just deleted): clear every
        // driver's Layer + source-buffer pointers rather than leaving them at
        // their previous values. An early return here left drivers holding a
        // dangling layer_ pointing at the freed Layer — PreviewDriver then read
        // layer_->layouts() on freed memory and crashed (LoadProhibited). A
        // driver with a null layer/buffer is a well-defined idle state.
        // Drivers read the composed outputBuffer_ when we composite (≥2 enabled
        // layers) or when the single layer needs a LUT map; otherwise the lone
        // no-LUT layer's buffer is handed directly (zero-copy fast path). Mirrors
        // the same decision loop() makes (outputBuffer_ is allocated iff this).
        // The source is the first *enabled* layer, never the disabled fallback
        // activeLayer() returns when all layers are off — with no enabled layer
        // buf stays null and every driver idles (its last frame is not re-sent).
        // A pinned setLayer() (layers_ null) is always the live source.
        Layer* const out = layers_ ? layers_->firstEnabledLayer() : layer_;
        const bool composing = layers_ && layers_->enabledLayerCount() > 1;
        Buffer* buf = out ? ((composing || out->lut().hasLUT()) ? &outputBuffer_
                                                               : &out->buffer())
                          : nullptr;
        for (uint8_t i = 0; i < childCount(); i++) {
            auto* drv = static_cast<DriverBase*>(child(i));
            drv->setSourceBuffer(buf);
            drv->setLayer(out);  // null when no enabled Layer; drivers must tolerate it
            drv->setCorrection(&correction_);  // physical drivers apply it; Preview ignores
        }
    }
};

} // namespace mm
