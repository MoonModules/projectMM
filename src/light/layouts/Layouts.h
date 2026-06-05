#pragma once

#include "core/MoonModule.h"
#include "light/light_types.h" // lengthType, nrOfLightsType

namespace mm {

// Callback for layout coordinate iteration — a layout walks its positions and
// invokes this per light with the physical index and (x,y,z). Owned by
// LayoutBase: it's the signature of forEachCoord, which every layout overrides.
using CoordCallback = void(*)(void* ctx, nrOfLightsType idx, lengthType x, lengthType y, lengthType z);

class LayoutBase : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Layout; }
    virtual nrOfLightsType lightCount() const = 0;
    virtual void forEachCoord(CoordCallback cb, void* ctx) const = 0;

    // Every layout control (grid width/height/depth, …) changes the physical light
    // count and therefore needs the pipeline-wide rebuild. See MoonModule::onUpdate.
    bool controlChangeTriggersBuildState(const char* /*controlName*/) const override { return true; }
};

// Top-level container for one or more `LayoutBase` children. Walks them in
// registration order, stitching per-child light indices into a single flat
// physical address space via `forEachCoord`. Shared by every Layer — there's
// one Layouts describing the physical setup, multiple Layers render into it.
class Layouts : public MoonModule {
public:
    const char* acceptsChildRoles() const override { return "layout"; }

    // Disabled children are skipped, same gate Layer/Layers/Drivers apply to their
    // children. Indices of subsequent enabled layouts shift down to close the gap —
    // disable Layout A and Layout B's lights move to indices 0..N. Users who need
    // a stable index-to-fixture mapping disable the driver, not the layout.
    //
    // Disabling the container itself reports zero lights and an empty iteration —
    // same effect as disabling every child, so the universal-gate intent ("enabled
    // on every module means: exclude my contribution") holds for the container too.
    // The Scheduler can't enforce this for us because Layouts has no loop() — the
    // work happens in these cold-path methods called from Layer::onBuildState
    // and Drivers::onBuildState.
    nrOfLightsType totalLightCount() const {
        if (!enabled()) return 0;
        nrOfLightsType total = 0;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (!child(i)->enabled()) continue;
            total += static_cast<LayoutBase*>(child(i))->lightCount();
        }
        return total;
    }

    void forEachCoord(CoordCallback cb, void* ctx) const {
        if (!enabled()) return;
        nrOfLightsType offset = 0;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (!child(i)->enabled()) continue;
            auto* layout = static_cast<LayoutBase*>(child(i));
            // Wrap callback to add physical index offset
            struct WrapCtx {
                CoordCallback cb;
                void* ctx;
                nrOfLightsType offset;
            };
            WrapCtx wctx{cb, ctx, offset};
            layout->forEachCoord([](void* wc, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
                auto* w = static_cast<WrapCtx*>(wc);
                w->cb(w->ctx, idx + w->offset, x, y, z);
            }, &wctx);
            offset += layout->lightCount();
        }
    }
};

} // namespace mm
