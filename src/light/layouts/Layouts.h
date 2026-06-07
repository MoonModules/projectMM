#pragma once

#include "core/MoonModule.h"
#include "light/light_types.h" // lengthType, nrOfLightsType

#include <cstdio> // std::snprintf for the status line

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

    // Status line: total physical lights + the physical bounding box (the extent
    // of all light coordinates). Both are derived facts the container owns — the
    // count is the driver buffer size, the box is the dense render extent. Shown
    // via the status slot (not controls) so it costs no spec-check entry and
    // renders generically. Recomputed only on a rebuild (cold path). A degenerate
    // setup (no lights / zero box) flags Warning so the UI shows it's empty.
    void onBuildState() override {
        const nrOfLightsType lights = totalLightCount();
        // One forEachCoord pass for the bounding box: max coordinate + 1 per axis.
        struct Extent { lengthType x, y, z; bool any; } e{0, 0, 0, false};
        forEachCoord([](void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
            auto* ex = static_cast<Extent*>(ctx);
            if (x > ex->x) ex->x = x;
            if (y > ex->y) ex->y = y;
            if (z > ex->z) ex->z = z;
            ex->any = true;
        }, &e);
        const lengthType w = e.any ? e.x + 1 : 0;
        const lengthType h = e.any ? e.y + 1 : 0;
        const lengthType d = e.any ? e.z + 1 : 0;
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%u lights · %u×%u×%u",
                      static_cast<unsigned>(lights),
                      static_cast<unsigned>(w), static_cast<unsigned>(h), static_cast<unsigned>(d));
        setStatus(statusBuf_, lights == 0 ? Severity::Warning : Severity::Status);
        MoonModule::onBuildState();
    }

private:
    char statusBuf_[40] = {};  // "65535 lights · 999×999×999" fits; owned (setStatus borrows)
};

} // namespace mm
