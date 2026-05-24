#pragma once

#include "core/MoonModule.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "platform/platform.h"

namespace mm {

// Top-level container for one or more `Layer` children. Each child Layer
// renders into its own buffer using the shared `Layouts` instance for physical
// topology. `Drivers` composes the resulting buffers (today's "first wins"
// placeholder, alpha-blend / additive in the composition follow-up).
//
// With one child Layer this is a thin pass-through: loop() runs the child
// Layer's loop() in order; behaviour matches the previous single-Layer
// pipeline byte-for-byte. The container itself owns no buffer.
class Layers : public MoonModule {
public:
    // Wire the shared Layouts. Propagates to every child Layer so their
    // onAllocateMemory() can size buffers from it. Idempotent — call again
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

    void loop() override {
        // Scheduler gates Layers itself via respectsEnabled() default.
        for (uint8_t i = 0; i < childCount(); i++) {
            if (!child(i)->enabled()) continue;
            uint32_t start = platform::micros();
            child(i)->loop();
            child(i)->addAccumUs(platform::micros() - start);
        }
    }

    // Single-Layer placeholder until composition lands: hand `Drivers` the
    // first enabled Layer to read for buffer + dimensions. Returns nullptr
    // when no Layer is registered (drivers handle that gracefully today).
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

private:
    Layouts* layouts_ = nullptr;
};

} // namespace mm
