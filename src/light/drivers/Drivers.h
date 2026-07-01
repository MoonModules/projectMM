#pragma once

#include "core/MoonModule.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layers/Layers.h"
#include "light/layers/BlendMap.h"
#include "light/drivers/Correction.h"
#include "light/Palette.h"   // the global active palette + its select control
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

    // The configured window (start light, count; count 0 = to end of buffer).
    // Public for tests pinning the slice arithmetic; production reads via
    // windowSlice(). See start_/count_ below.
    uint16_t windowStart() const { return start_; }
    uint16_t windowCount() const { return count_; }
    // Set the window directly (the UI sets it via the start/count controls; this
    // is for code-wiring a driver's slice and for tests). Takes effect on the next
    // config parse / loop, like a control edit.
    void setWindow(uint16_t start, uint16_t count) { start_ = start; count_ = count; }
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

    // --- Shared source-buffer window (start, count) ---------------------------
    // Every driver reads the SAME shared source buffer (Drivers hands the one
    // Buffer* to each child) and outputs a contiguous slice of it: lights
    // [start_, start_+count_). This is how light distribution is made *explicit*
    // and order-independent — a second driver on a different slice (e.g. an
    // onboard status LED at index 0, the main strip from index 1) just sets its
    // own window, rather than the buffer being split by driver order. `count_`==0
    // means "the rest of the buffer from start_" (the common whole-buffer case).
    // NetworkSendDriver's universe maps onto the same window; the LED drivers'
    // pins/ledsPerPin distribute lights *within* the window.
    uint16_t start_ = 0;
    uint16_t count_ = 0;   // 0 = to end of buffer

    // Add the two window controls — call from a driver's onBuildControls(). Kept
    // a helper (not auto-added) so a driver opts in by calling it where its other
    // controls go, keeping control *order* in the driver's hands.
    void addWindowControls() {
        controls_.addUint16("start", start_);
        controls_.addUint16("count", count_);
    }

    // True if `name` is one of the window controls — a driver folds this into its
    // controlChangeTriggersBuildState() so editing the slice re-runs its config.
    static bool isWindowControl(const char* name) {
        return std::strcmp(name, "start") == 0 || std::strcmp(name, "count") == 0;
    }

    // Resolve the window against a buffer of `bufN` lights: writes the clamped
    // first light to `outStart` and the slice length to `outLen` (0 if the window
    // starts past the end). The textbook [start, start+count) clamp — every
    // driver calls this instead of reading from light 0.
    void windowSlice(nrOfLightsType bufN, nrOfLightsType& outStart,
                     nrOfLightsType& outLen) const {
        outStart = start_ < bufN ? start_ : bufN;
        const nrOfLightsType avail = static_cast<nrOfLightsType>(bufN - outStart);
        outLen = (count_ == 0 || count_ > avail) ? avail
                                                 : static_cast<nrOfLightsType>(count_);
    }

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
    uint8_t palette = 0;      // index into mm::palettes::kBuiltins; the global active palette effects read

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
        controls_.addPalette("palette", palette, mm::paletteOptions, mm::palettes::kCount);
        MoonModule::onBuildControls();  // cascade to driver children
    }

    // Brightness / light-preset changes only rebuild the (cheap) correction LUT — no
    // pipeline realloc. This is what keeps the brightness slider fluent: controlChangeTriggersBuildState
    // stays false for Drivers, so handleSetControl skips scheduler_->buildState().
    void onUpdate(const char* controlName) override {
        if (std::strcmp(controlName, "palette") == 0) {
            Palettes::setActive(palette);   // rebuild the active 16-entry lookup (cheap, off the hot path)
            return;
        }
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
        Palettes::setActive(palette);   // seed the global active palette from the persisted index
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

    // First output light as RGB — the live colour of pixel 0, read from whichever buffer
    // loop() is currently driving (the composited outputBuffer_ when allocated, else the
    // first enabled layer's own buffer — the zero-copy single-layer path). The WLED shim
    // tints the app's device card with this. RGB is the buffer's logical channel order
    // (0,1,2); the per-strip wire reorder is applied later by the physical drivers, not here.
    bool firstOutputRgb(uint8_t out[3]) const override {
        const Buffer* src = nullptr;
        if (outputBuffer_.data()) src = &outputBuffer_;
        else if (Layer* l = layers_ ? layers_->firstEnabledLayer() : layer_; l && l->buffer().data())
            src = &l->buffer();
        if (!src || src->count() == 0 || src->channelsPerLight() < 3) return false;
        const uint8_t* p = src->data();
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
        return true;
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
            // Geometry uses layer_ (activeLayer()'s fallback — valid even when every
            // layer is disabled) so a PreviewDriver keeps its width/height/depth and
            // coordinate table; buf above uses the enabled source only, so output
            // still idles (no stale frame) when nothing is enabled. layer_ is null
            // only when no Layer is registered at all (the documented idle state).
            drv->setLayer(layer_);
            drv->setCorrection(&correction_);  // physical drivers apply it; Preview ignores
        }
    }
};

} // namespace mm
