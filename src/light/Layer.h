#pragma once

#include "light/Buffer.h"
#include "light/LayoutGroup.h"
#include "light/EffectBase.h"
#include "platform/platform.h"

#include <array>

namespace mm {

class Layer : public MoonModule {
public:
    void setLayoutGroup(LayoutGroup* lg) { layoutGroup_ = lg; }
    void setChannelsPerLight(uint8_t cpl) { channelsPerLight_ = cpl; }

    void addEffect(EffectBase* effect) {
        if (effectCount_ >= effects_.size()) return;
        effect->setParent(this);
        effect->setLayer(this);
        effects_[effectCount_++] = effect;
    }

    void setup() override {
        for (uint8_t i = 0; i < effectCount_; i++) {
            effects_[i]->setup();
        }
    }

    void onBuildControls() override {
        for (uint8_t i = 0; i < effectCount_; i++) {
            effects_[i]->onBuildControls();
        }
    }

    void onAllocateMemory() override {
        if (!layoutGroup_) return;
        nrOfLightsType total = layoutGroup_->totalLightCount();
        if (total == 0) return;

        // Compute dimensions from the first layout (for effects)
        // For now, use the full layout dimensions
        width_ = 0;
        height_ = 0;
        depth_ = 0;
        struct DimCtx { lengthType maxX, maxY, maxZ; };
        DimCtx dctx{0, 0, 0};
        layoutGroup_->forEachCoord([](void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
            auto* d = static_cast<DimCtx*>(ctx);
            if (x > d->maxX) d->maxX = x;
            if (y > d->maxY) d->maxY = y;
            if (z > d->maxZ) d->maxZ = z;
        }, &dctx);
        width_ = dctx.maxX + 1;
        height_ = dctx.maxY + 1;
        depth_ = dctx.maxZ + 1;

        buffer_.allocate(total, channelsPerLight_);
    }

    void loop() override {
        elapsed_ = platform::millis();
        buffer_.clear();
        for (uint8_t i = 0; i < effectCount_; i++) {
            effects_[i]->loop();
        }
    }

    Buffer& buffer() { return buffer_; }
    const Buffer& buffer() const { return buffer_; }

    lengthType width() const { return width_; }
    lengthType height() const { return height_; }
    lengthType depth() const { return depth_; }
    uint8_t channelsPerLight() const { return channelsPerLight_; }
    uint32_t elapsed() const { return elapsed_; }

private:
    LayoutGroup* layoutGroup_ = nullptr;
    std::array<EffectBase*, 4> effects_{};
    uint8_t effectCount_ = 0;
    Buffer buffer_;
    uint8_t channelsPerLight_ = 3;
    lengthType width_ = 0;
    lengthType height_ = 0;
    lengthType depth_ = 0;
    uint32_t elapsed_ = 0;
};

// EffectBase accessor implementations
inline uint8_t* EffectBase::buffer() { return layer_->buffer().data(); }
inline lengthType EffectBase::width() const { return layer_->width(); }
inline lengthType EffectBase::height() const { return layer_->height(); }
inline lengthType EffectBase::depth() const { return layer_->depth(); }
inline uint8_t EffectBase::channelsPerLight() const { return layer_->channelsPerLight(); }
inline nrOfLightsType EffectBase::nrOfLights() const { return layer_->buffer().count(); }
inline uint32_t EffectBase::elapsed() const { return layer_->elapsed(); }

} // namespace mm
