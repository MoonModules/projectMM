#pragma once

#include "core/MoonModule.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "platform/platform.h"

namespace mm {

// Top-level container for one or more `Layer` children. Each child Layer
// renders into its own buffer using the shared `Layouts` instance for physical
// topology. `Drivers` composites the resulting buffers in container order
// (bottom→top) per each Layer's blendMode + opacity.
//
// With one child Layer this is a thin pass-through: loop() runs the child
// Layer's loop() in order; behaviour matches the single-Layer pipeline
// byte-for-byte (Drivers takes its single-layer fast path). The container
// itself owns no buffer — the composite buffer lives in Drivers.
class Layers : public MoonModule {
public:
    const char* acceptsChildRoles() const override { return "layer"; }

    // Wire the shared Layouts. Propagates to every child Layer so their
    // onBuildState() can size buffers from it. Idempotent — call again
    // after adding a Layer child to wire the new one. Non-Layer children
    // (UI shouldn't allow them; engine doesn't enforce — yet) are skipped
    // rather than miscast, so a stray child can't UB the cast.
    void setLayouts(Layouts* l) {
        layouts_ = l;
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (!c || c->role() != ModuleRole::Layer) continue;
            static_cast<Layer*>(c)->setLayouts(layouts_);
        }
    }

    Layouts* layouts() const { return layouts_; }

    // Re-wire children before they build their state, so a Layer added via the
    // API (clear_children + add_module) gets the shared Layouts without anyone
    // re-running main.cpp's setLayouts. Then chain to base to build the children.
    void onBuildState() override {
        setLayouts(layouts_);
        MoonModule::onBuildState();
    }

    // Role-filtered loop propagation: only tick children that are Layers.
    // The factory / UI shouldn't allow non-Layer children of a Layers
    // container, but if one slips in (test fixture, hand-crafted config),
    // ticking it through Layers would run its loop at the wrong tree
    // depth (e.g. an Effect that should be ticked inside a Layer). Matches
    // the role-filter precedent in setLayouts / activeLayer above.
    void loop() override {
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (!c || c->role() != ModuleRole::Layer) continue;
            if (c->respectsEnabled() && !c->enabled()) continue;
            uint32_t start = platform::micros();
            c->loop();
            c->addAccumUs(platform::micros() - start);
        }
    }

    // The first enabled Layer — `Drivers` reads it for physical dimensions
    // (every layer composites into the same physical space, so any one answers
    // width/height/depth). Also the source for the single-layer fast path.
    // Returns nullptr when no Layer is registered (drivers handle that gracefully).
    // Non-Layer children are skipped — same guard as setLayouts above.
    Layer* activeLayer() const {
        MoonModule* fallback = nullptr;
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (!c || c->role() != ModuleRole::Layer) continue;
            if (!fallback) fallback = c;
            if (c->enabled()) return static_cast<Layer*>(c);
        }
        return static_cast<Layer*>(fallback);  // nullptr if no Layer children
    }

    // The first *enabled* Layer, or nullptr when none is enabled. Distinct from
    // activeLayer(), which falls back to a disabled registered Layer so geometry
    // (width/height/depth) stays queryable while everything is toggled off. Output
    // selection must use *this* one: handing a disabled layer's stale buffer to the
    // drivers would keep emitting its last frame instead of going idle.
    Layer* firstEnabledLayer() const {
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (!c || c->role() != ModuleRole::Layer || !c->enabled()) continue;
            return static_cast<Layer*>(c);
        }
        return nullptr;
    }

    // Count of enabled Layer children — Drivers uses it to pick the single-layer
    // fast path (==1) vs the composite path (>1), and to know if anything renders.
    uint8_t enabledLayerCount() const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (c && c->role() == ModuleRole::Layer && c->enabled()) n++;
        }
        return n;
    }

    // Walk enabled Layers in container (composition) order — the order Drivers
    // blends them, bottom (first) to top (last). `cb(layer, isFirst)`: isFirst
    // marks the bottom layer (clears the buffer; the rest blend onto it).
    template <typename Fn>
    void forEachEnabledLayer(Fn cb) const {
        bool first = true;
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (!c || c->role() != ModuleRole::Layer || !c->enabled()) continue;
            cb(static_cast<Layer*>(c), first);
            first = false;
        }
    }

private:
    Layouts* layouts_ = nullptr;
};

} // namespace mm
