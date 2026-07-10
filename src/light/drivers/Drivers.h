#pragma once

#include "light/drivers/DriverBase.h"  // DriverBase — the Drivers container casts its children to it
#include "core/MoonModule.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layers/Layers.h"
#include "light/layers/BlendMap.h"
#include "light/drivers/Correction.h"
#include "light/Palette.h"   // the global active palette + its select control
#include "core/LightSummary.h"   // the POD published for the domain-neutral WLED/MQTT consumers
#include "core/FilesystemModule.h" // noteDirty for one-time legacy setting migrations
#include "platform/platform.h"

#include <cstring>  // std::strcmp in onUpdate

namespace mm {

/// Top-level container for one or more drivers — the consumer side of the pipeline.
/// Owns the shared output buffer (when memory allows) and performs blend+map from
/// every layer's buffer into it each frame.
///
/// **Naming convention.** Capital `Drivers` is the container class; lowercase
/// "driver"/"drivers" is the English singular/plural for individual `DriverBase`
/// children. Capitalisation disambiguates "the Drivers container" from "two drivers
/// running" (same rule for `Layouts`/layout and `Layers`/layer).
///
/// **Shared output buffer.** Necessary because blend+map writes to arbitrary physical
/// positions via LUT — the output is not filled sequentially, so a driver cannot read
/// chunk-by-chunk until the full buffer is populated. It uses the same `Buffer` type a
/// Layer does, sized by the Layouts container. Exception: when exactly one layer is
/// enabled AND its mapping is 1:1 unshuffled (no LUT — grid layout, no serpentine),
/// Drivers skips its own buffer and lets drivers read directly from the layer's buffer
/// (the zero-copy fast path, at the cost of parallelism).
///
/// **Multi-layer composition.** When two or more layers are enabled, Drivers composites
/// them into the shared output buffer each frame in Layers container order (bottom→top,
/// via `forEachEnabledLayer`). The bottom layer clears and overwrites the buffer; each
/// layer above blends onto the accumulated frame per its own `blendMode` and `opacity`
/// (the inert per-Layer controls). Drivers owns the orchestration because only it sees
/// the stack order and the output buffer; the layers carry only the parameters. The
/// per-pixel blend math lives in `blendMap` (integer-only, per the hot-path rule). A
/// full-opacity overwrite/additive layer pays no alpha arithmetic, so per-frame cost
/// scales with the enabled-layer count. With a single enabled layer there is no
/// composite: the fast path applies (no-LUT → zero-copy; with a LUT → one blend+map pass
/// into the output buffer).
///
/// **Output correction.** Drivers owns the shared output-correction state (a
/// `Correction`: brightness LUT, channel-order table, output channel count, derive-white
/// flag) and exposes `brightness`, `lightPreset`, and the global `palette` controls; each
/// *physical* driver child applies the correction per-light as it reads the source buffer,
/// while Preview ignores it (shows the raw logical buffer). `onUpdate` rebuilds the
/// correction on a `brightness`/`lightPreset` change and hands each child a
/// `const Correction*`. Every driver sees the same composited output. Palette model +
/// names follow FastLED's, credited as prior art; implementation in `src/light/Palette.h`.
///
/// **Per-driver source window (`start` / `count`).** A window-aware output driver reads
/// the shared source buffer and outputs a contiguous slice of it — its *window* — making
/// light distribution explicit and order-independent: each driver names its own slice, so
/// reordering drivers does not change which lights each outputs (only tick order). The
/// motivating case: an onboard status LED with window `[0, 1)` and a main strip with
/// window `[1, …)` as two driver instances on the same buffer, neither stealing the
/// other's lights. `DriverBase::addWindowControls()` opts a driver in (see there); a driver
/// that outputs the whole buffer (such as PreviewDriver) simply doesn't call it.
///
/// **Prior art:** MoonLight's PhysicalLayer — owns `channelsD` (display buffer),
/// `compositeLayers()` maps virtualChannels → channelsD, parallelism via a semaphore
/// (driver signals completion, compositor writes)
/// (https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Layers/PhysicalLayer.h).
/// @card Drivers.png
class Drivers : public MoonModule {
public:
    const char* acceptsChildRoles() const override { return "driver"; }

    /// The live light-pipeline summary (light count, channels), for the domain-neutral core
    /// consumers (WLED /json shim, MQTT). Static so a factory-created consumer reaches it without
    /// a wiring inject — the same shape as `AudioModule::latestFrame()`. Points at the active
    /// Drivers' summary (set in onBuildState, vacated in teardown); a default all-zero summary
    /// when no Drivers is in the tree, so a consumer always reads a valid POD, never null.
    static const LightSummary* latestSummary() {
        static const LightSummary kNone{};
        return active_ ? &active_->summary_ : &kNone;
    }

    // Vacate the summary seat when this Drivers is removed, so latestSummary() falls back to the
    // all-zero default rather than a dangling pointer (the robustness rule). MoonModule::teardown
    // recurses to children.
    void teardown() override {
        if (active_ == this) active_ = nullptr;
        MoonModule::teardown();
    }

    /// Global brightness (0–255). Scales every channel through a 256-entry LUT
    /// (`(v × brightness) / 255`); changing it rebuilds only the LUT on the cheap
    /// `onUpdate` tier — no pipeline realloc, so the slider is fluent. Gamma /
    /// white-balance fold into this LUT later as a per-channel R/G/B split.
    ///
    /// Default low (≈8%). A fresh device with LEDs wired but no power budget set
    /// (such as a strip on USB 5V) draws far less at 20 than at full white, so the
    /// first boot can't brown out the board before the user sets a safe level.
    /// The user raises it via the brightness control once their supply is known.
    uint8_t brightness = 20;
    /// Master power. `on=false` outputs black without touching `brightness`, so toggling back
    /// to `on=true` restores the exact level. Implemented by scaling the correction LUT to zero
    /// (effectiveBrightness()) — the same cold-path rebuild brightness uses, no hot-path branch.
    /// This is the single power control every consumer drives: the UI toggle, IR's on/off action,
    /// the WLED app / Home Assistant (`{"on":…}`), and MQTT/Homebridge all set THIS control through
    /// `Scheduler::setControl` — define-once, reuse everywhere (the first slice of a global
    /// lights-control surface). Default on so a freshly-flashed board lights up.
    bool on = true;
    /// Physical wire format: channel order and whether the light is RGBW (index into
    /// `kLightPresetOptions`; RGB orders plus every RGBW byte permutation). RGBW
    /// presets make each driver emit 4 channels per light with white derived as
    /// `min(R,G,B)` from the (brightness-scaled) RGB.
    ///
    /// GRB (index 2) is the wire order of WS2812/SK6812 strips — the common case, so a
    /// freshly-flashed board with a strip attached shows correct colours out of the box.
    /// Only the physical output drivers apply this reorder; PreviewDriver reads the RGB
    /// source buffer directly, so the simulator is unaffected. RGB-ordered outputs (some
    /// ArtNet/network sinks) flip it back.
    uint8_t lightPreset = 2;  ///< index into kLightPresetOptions; 2 = GRB
    /// The global active colour palette (index into `mm::palettes::kBuiltins`;
    /// `Rainbow`, `Party`, `Lava`, `Ocean`, …). Palette-driven effects read it via
    /// `Palettes::active()` and colour their pixels through `colorFromPalette(index)`, so
    /// changing this recolours every such effect live. The select index expands the chosen
    /// gradient into the active 16-entry palette on `onUpdate` (cheap, off the hot path).
    uint8_t palette = 0;

    /// Hidden migration marker for the physical-light preset schema. Older builds
    /// had only RGB plus two RGBW presets, so a saved numeric index can mean the
    /// wrong wire byte order after the expanded RGBW preset list lands.
    uint8_t lightPresetSchema = 0;

    void setLegacyLightDefaults(uint8_t preset, uint8_t brightnessCap = 255) {
        legacyLightPresetTarget_ = preset;
        legacyBrightnessCap_ = brightnessCap;
    }

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

    // The brightness actually fed to the LUT: 0 when powered off, else the set brightness. Keeping
    // `on` and `brightness` independent means "off" never clobbers the level the user chose.
    uint8_t effectiveBrightness() const { return on ? brightness : 0; }

    void onBuildControls() override {
        controls_.addBool("on", on);   // master power — first so it renders at the top of the card
        controls_.addUint8("brightness", brightness, 0, 255);
        controls_.addSelect("lightPreset", lightPreset, kLightPresetOptions, kLightPresetCount);
        controls_.addUint8("lightPresetSchema", lightPresetSchema, 0, kLightPresetSchemaVersion);
        controls_.setHidden(controls_.count() - 1, true);
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
        if (std::strcmp(controlName, "on") == 0 ||
            std::strcmp(controlName, "brightness") == 0 ||
            std::strcmp(controlName, "lightPreset") == 0) {
            correction_.rebuild(effectiveBrightness(), static_cast<LightPreset>(lightPreset));
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
        migrateLegacyLightDefaults();
        correction_.rebuild(effectiveBrightness(), static_cast<LightPreset>(lightPreset));
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
        // Publish the light-pipeline summary for the domain-neutral core consumers (the WLED
        // /json shim, MQTT) via the static latestSummary() pull. `out` is the composite extent;
        // no enabled layer → zero lights. One POD, overwritten in place on each rebuild.
        summary_.lightCount = out ? static_cast<uint32_t>(out->physicalLightCount()) : 0;
        summary_.channelsPerLight = out ? out->channelsPerLight() : 3;
        active_ = this;
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

    // Published to core via latestSummary(). active_ points at the Drivers whose summary is live
    // (mirrors AudioModule::active_): set in onBuildState, cleared in teardown so a removed Drivers
    // stops being read; only one Drivers exists in the pinned tree, so no re-election dance is needed.
    LightSummary summary_;
    inline static Drivers* active_ = nullptr;

    static constexpr uint8_t kLightPresetSchemaVersion = 1;
    static constexpr uint8_t kNoLegacyLightPresetTarget = 255;
    uint8_t legacyLightPresetTarget_ = kNoLegacyLightPresetTarget;
    uint8_t legacyBrightnessCap_ = 255;

    void migrateLegacyLightDefaults() {
        if (lightPresetSchema >= kLightPresetSchemaVersion) return;

        bool changed = false;
        if (legacyLightPresetTarget_ < kLightPresetCount &&
            lightPreset != legacyLightPresetTarget_) {
            lightPreset = legacyLightPresetTarget_;
            changed = true;
        }
        if (brightness > legacyBrightnessCap_) {
            brightness = legacyBrightnessCap_;
            changed = true;
        }
        lightPresetSchema = kLightPresetSchemaVersion;
        changed = true;

        if (changed) {
            markDirty();
            FilesystemModule::noteDirty();
        }
    }

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
