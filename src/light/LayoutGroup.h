#pragma once

#include "core/MoonModule.h"
#include "core/types.h"

#include <array>

namespace mm {

class LayoutBase : public MoonModule {
public:
    virtual nrOfLightsType lightCount() const = 0;
    virtual void forEachCoord(CoordCallback cb, void* ctx) const = 0;
};

class LayoutGroup : public MoonModule {
public:
    void addLayout(LayoutBase* layout) {
        if (layoutCount_ >= layouts_.size()) return;
        layout->setParent(this);
        layouts_[layoutCount_++] = layout;
    }

    nrOfLightsType totalLightCount() const {
        nrOfLightsType total = 0;
        for (uint8_t i = 0; i < layoutCount_; i++) {
            total += layouts_[i]->lightCount();
        }
        return total;
    }

    void forEachCoord(CoordCallback cb, void* ctx) const {
        nrOfLightsType offset = 0;
        for (uint8_t i = 0; i < layoutCount_; i++) {
            auto* layout = layouts_[i];
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

    uint8_t layoutCount() const { return layoutCount_; }

private:
    std::array<LayoutBase*, 4> layouts_{};
    uint8_t layoutCount_ = 0;
};

} // namespace mm
