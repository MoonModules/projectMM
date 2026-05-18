#pragma once

#include "core/MoonModule.h"
#include <cstddef>
#include <cstdint>

namespace mm::light {

// Callback type for coordinate iteration (non-template virtual dispatch)
using CoordCallback = void(*)(void* ctx, uint32_t idx, int16_t x, int16_t y, int16_t z);

// Interface that layouts must provide for LayoutGroup iteration
class LayoutBase {
public:
    virtual ~LayoutBase() = default;
    virtual size_t pixelCount() const = 0;
    virtual void forEachCoord(CoordCallback cb, void* ctx) const = 0;
};

// LayoutGroup groups layouts. Shared across all layers and drivers.
class LayoutGroup : public MoonModule {
public:
    const char* name() const override { return "LayoutGroup"; }

    static constexpr uint8_t MAX_LAYOUTS = 8;

    void addLayout(LayoutBase* layout) {
        if (layoutCount_ < MAX_LAYOUTS) {
            layouts_[layoutCount_++] = layout;
        }
    }

    size_t totalPixelCount() const {
        size_t total = 0;
        for (uint8_t i = 0; i < layoutCount_; ++i) {
            total += layouts_[i]->pixelCount();
        }
        return total;
    }

    // Iterate all coordinates across all layouts.
    // Physical indices are offset so they don't overlap between layouts.
    void forEachCoord(CoordCallback cb, void* ctx) const {
        uint32_t offset = 0;
        for (uint8_t i = 0; i < layoutCount_; ++i) {
            struct State { CoordCallback cb; void* ctx; uint32_t offset; };
            State state{cb, ctx, offset};
            layouts_[i]->forEachCoord(
                [](void* s, uint32_t idx, int16_t x, int16_t y, int16_t z) {
                    auto* st = static_cast<State*>(s);
                    st->cb(st->ctx, static_cast<uint16_t>(idx + st->offset), x, y, z);
                },
                &state
            );
            offset += static_cast<uint16_t>(layouts_[i]->pixelCount());
        }
    }

    uint8_t layoutCount() const { return layoutCount_; }

private:
    LayoutBase* layouts_[MAX_LAYOUTS] = {};
    uint8_t layoutCount_ = 0;
};

} // namespace mm::light
