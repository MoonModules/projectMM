#pragma once

#include "core/MoonModule.h"
#include "core/types.h"

namespace mm {

class LayoutBase : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Layout; }
    virtual nrOfLightsType lightCount() const = 0;
    virtual void forEachCoord(CoordCallback cb, void* ctx) const = 0;
};

class LayoutGroup : public MoonModule {
public:
    nrOfLightsType totalLightCount() const {
        nrOfLightsType total = 0;
        for (uint8_t i = 0; i < childCount(); i++) {
            total += static_cast<LayoutBase*>(child(i))->lightCount();
        }
        return total;
    }

    void forEachCoord(CoordCallback cb, void* ctx) const {
        nrOfLightsType offset = 0;
        for (uint8_t i = 0; i < childCount(); i++) {
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
