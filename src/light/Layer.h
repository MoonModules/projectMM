#pragma once

#include "light/Buffer.h"
#include "light/LayoutGroup.h"
#include "light/EffectBase.h"
#include "light/MappingLUT.h"
#include "light/ModifierBase.h"
#include "platform/platform.h"

#include <cstdio>

namespace mm {

class Layer : public MoonModule {
public:
    void setLayoutGroup(LayoutGroup* lg) { layoutGroup_ = lg; }
    void setChannelsPerLight(uint8_t cpl) { channelsPerLight_ = cpl; }

    void onAllocateMemory() override {
        if (!layoutGroup_) return;
        nrOfLightsType physicalCount = layoutGroup_->totalLightCount();
        if (physicalCount == 0) return;

        // Compute physical dimensions from layout
        struct DimCtx { lengthType maxX, maxY, maxZ; };
        DimCtx dctx{0, 0, 0};
        layoutGroup_->forEachCoord([](void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
            auto* d = static_cast<DimCtx*>(ctx);
            if (x > d->maxX) d->maxX = x;
            if (y > d->maxY) d->maxY = y;
            if (z > d->maxZ) d->maxZ = z;
        }, &dctx);
        physicalWidth_ = dctx.maxX + 1;
        physicalHeight_ = dctx.maxY + 1;
        physicalDepth_ = dctx.maxZ + 1;

        rebuildLUT();

        // Children allocate after LUT is built (effects need buffer dimensions)
        MoonModule::onAllocateMemory();
    }

    void loop() override {
        elapsed_ = platform::millis();
        buffer_.clear();
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() == ModuleRole::Effect) {
                uint32_t start = platform::micros();
                child(i)->loop();
                child(i)->addAccumUs(platform::micros() - start);
            }
        }
    }

    Buffer& buffer() { return buffer_; }
    const Buffer& buffer() const { return buffer_; }
    const MappingLUT& lut() const { return lut_; }

    // Effects see logical dimensions
    lengthType width() const { return width_; }
    lengthType height() const { return height_; }
    lengthType depth() const { return depth_; }
    uint8_t channelsPerLight() const { return channelsPerLight_; }
    uint32_t elapsed() const { return elapsed_; }

    nrOfLightsType physicalLightCount() const {
        return layoutGroup_ ? layoutGroup_->totalLightCount() : 0;
    }

    bool lutSkipped() const { return lutSkipped_; }

    // Precondition: physicalWidth_/Height_/Depth_ must be set (call from onAllocateMemory)
    void rebuildLUT() {
        lutSkipped_ = false;

        // Find first modifier (if any)
        ModifierBase* mod = nullptr;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() == ModuleRole::Modifier) {
                mod = static_cast<ModifierBase*>(child(i));
                break;
            }
        }

        if (!mod) {
            // No modifiers: 1:1 unshuffled, logical == physical
            width_ = physicalWidth_;
            height_ = physicalHeight_;
            depth_ = physicalDepth_;
            nrOfLightsType count = static_cast<nrOfLightsType>(width_) * height_ * depth_;
            lut_.setIdentity(count);
            allocateBuffer(count);
            return;
        }

        // Apply first static modifier to compute logical dimensions
        mod->logicalDimensions(physicalWidth_, physicalHeight_, physicalDepth_,
                               width_, height_, depth_);

        nrOfLightsType logicalCount = static_cast<nrOfLightsType>(width_) * height_ * depth_;
        nrOfLightsType physicalCount = static_cast<nrOfLightsType>(physicalWidth_) * physicalHeight_ * physicalDepth_;

        // Estimate max destinations from modifier's multiplier
        nrOfLightsType maxDest = logicalCount * mod->maxMultiplier();
        if (maxDest > physicalCount * 2) maxDest = physicalCount * 2;

        size_t lutBytes = MappingLUT::estimateBytes(logicalCount, maxDest);
        if (!canAllocate(lutBytes)) {
            // Not enough memory for LUT — degrade to 1:1
            std::printf("  DEGRADE  LUT skipped (need %u, free %u)\n",
                        static_cast<unsigned>(lutBytes),
                        static_cast<unsigned>(platform::freeHeap()));
            lutSkipped_ = true;
            width_ = physicalWidth_;
            height_ = physicalHeight_;
            depth_ = physicalDepth_;
            lut_.setIdentity(physicalCount);
            allocateBuffer(physicalCount);
            return;
        }

        lut_.build(logicalCount, maxDest);

        // Fill LUT by iterating all logical coordinates
        nrOfLightsType physicals[8];
        nrOfLightsType count;
        nrOfLightsType logIdx = 0;

        for (lengthType z = 0; z < depth_; z++) {
            for (lengthType y = 0; y < height_; y++) {
                for (lengthType x = 0; x < width_; x++) {
                    count = 0;
                    mod->mapToPhysical(x, y, z, physicalWidth_, physicalHeight_, physicalDepth_,
                                       physicals, count, 8);
                    lut_.setMapping(logIdx, physicals, count);
                    logIdx++;
                }
            }
        }

        lut_.finalize();
        allocateBuffer(logicalCount);
    }

private:
    LayoutGroup* layoutGroup_ = nullptr;
    Buffer buffer_;
    MappingLUT lut_;
    uint8_t channelsPerLight_ = 3;
    bool lutSkipped_ = false;
    lengthType physicalWidth_ = 0;
    lengthType physicalHeight_ = 0;
    lengthType physicalDepth_ = 0;
    lengthType width_ = 0;  // logical (what effects see)
    lengthType height_ = 0;
    lengthType depth_ = 0;
    uint32_t elapsed_ = 0;

    // Check if heap can afford an allocation (returns true if unlimited or enough budget)
    static bool canAllocate(size_t bytesNeeded) {
        size_t availableHeap = platform::freeHeap();
        if (availableHeap == 0) return true; // desktop: unlimited
        size_t internalHeap = platform::freeInternalHeap();
        if (internalHeap > 0 && internalHeap <= HEAP_RESERVE) return false;
        size_t budget = availableHeap > HEAP_RESERVE ? availableHeap - HEAP_RESERVE : 0;
        return budget >= bytesNeeded && platform::maxAllocBlock() >= bytesNeeded;
    }

    void allocateBuffer(nrOfLightsType count) {
        // Try to allocate buffer, halve dimensions if needed
        while (count > 0) {
            size_t needed = static_cast<size_t>(count) * channelsPerLight_;
            if (canAllocate(needed)) {
                buffer_.allocate(count, channelsPerLight_);
                setDynamicBytes(buffer_.bytes() + lut_.memoryUsed());
                return;
            }
            // Halve: reduce to sqrt of count (halve each dimension)
            width_ = width_ > 1 ? width_ / 2 : 1;
            height_ = height_ > 1 ? height_ / 2 : 1;
            depth_ = depth_ > 1 ? depth_ / 2 : 1;
            count = static_cast<nrOfLightsType>(width_) * height_ * depth_;
            std::printf("  DEGRADE  buffer too large, reducing to %dx%dx%d\n",
                        static_cast<int>(width_), static_cast<int>(height_), static_cast<int>(depth_));
            if (width_ <= 8 && height_ <= 8) break; // minimum
        }
        buffer_.allocate(count, channelsPerLight_);
        setDynamicBytes(buffer_.bytes() + lut_.memoryUsed());
    }
};

// EffectBase accessor implementations
inline Layer* EffectBase::layer() const { return static_cast<Layer*>(parent()); }
inline uint8_t* EffectBase::buffer() { return layer()->buffer().data(); }
inline lengthType EffectBase::width() const { return layer()->width(); }
inline lengthType EffectBase::height() const { return layer()->height(); }
inline lengthType EffectBase::depth() const { return layer()->depth(); }
inline uint8_t EffectBase::channelsPerLight() const { return layer()->channelsPerLight(); }
inline nrOfLightsType EffectBase::nrOfLights() const { return layer()->buffer().count(); }
inline uint32_t EffectBase::elapsed() const { return layer()->elapsed(); }

} // namespace mm
