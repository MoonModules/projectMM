#pragma once

#include "light/Buffer.h"
#include "light/EffectBase.h"
#include "light/LayoutGroup.h"
#include "light/MappingLUT.h"
#include "light/RenderContext.h"
#include "light/modules/modifiers/MirrorModifier.h"
#include "light/modules/modifiers/RotateModifier.h"
#include <cstdint>

namespace mm::light {

// A Layer owns a buffer, a mapping LUT, effects, and modifiers.
// It references a shared LayoutGroup for coordinate data.
class Layer {
public:
    static constexpr uint8_t MAX_EFFECTS = 4;
    static constexpr uint8_t MAX_MODIFIERS = 4;

    void setLayoutGroup(LayoutGroup* fixture) { fixture_ = fixture; }

    void setEffect(MoonModule* effect) {
        // Convenience: set single effect (clears list, adds one)
        effectCount_ = 0;
        addEffect(effect);
    }

    void addEffect(MoonModule* effect) {
        if (effect && effectCount_ < MAX_EFFECTS) {
            effects_[effectCount_++] = effect;
        }
    }

    void clearEffects() { effectCount_ = 0; }

    void addModifier(MoonModule* mod) {
        if (modifierCount_ < MAX_MODIFIERS) {
            modifiers_[modifierCount_++] = mod;
        }
    }

    void clearModifiers() { modifierCount_ = 0; }

    // Rebuild LUT from fixture coordinates + static modifiers.
    // Called on fixture/layout/modifier changes (cold path).
    void rebuildLUT() {
        if (!fixture_) return;

        // Scan physical dimensions
        dims_[0] = 0; dims_[1] = 0; dims_[2] = 0;
        if (fixture_->layoutCount() > 0) {
            fixture_->forEachCoord(
                [](void* ctx, uint32_t /*idx*/, int16_t x, int16_t y, int16_t z) {
                    auto* dims = static_cast<int16_t*>(ctx);
                    if (x + 1 > dims[0]) dims[0] = x + 1;
                    if (y + 1 > dims[1]) dims[1] = y + 1;
                    if (z + 1 > dims[2]) dims[2] = z + 1;
                },
                dims_
            );
        }
        int16_t physW = dims_[0], physH = dims_[1], physD = dims_[2];

        // Compute logical dimensions (reduced by mirror modifiers)
        int16_t logW = physW, logH = physH, logD = physD;
        MirrorModifier* mirror = nullptr;
        for (uint8_t m = 0; m < modifierCount_; ++m) {
            auto* mir = dynamic_cast<MirrorModifier*>(modifiers_[m]);
            if (mir) {
                mirror = mir;
                logW = mir->logicalWidth(logW);
                logH = mir->logicalHeight(logH);
                logD = mir->logicalDepth(logD);
            }
        }

        // Store logical dims for effects
        logDims_[0] = logW;
        logDims_[1] = logH;
        logDims_[2] = logD;

        size_t logicalCount = static_cast<size_t>(logW) * logH * logD;
        uint8_t mult = mirror ? mirror->multiplier() : 1;
        size_t totalDest = logicalCount * mult;

        buffer_.allocate(logicalCount);
        lut_.allocate(logicalCount, totalDest);

        // Build LUT: for each logical pixel, compute physical destinations
        uint16_t physDests[8]; // max 2x2x2 = 8
        size_t logIdx = 0;
        for (int16_t lz = 0; lz < logD; ++lz) {
            for (int16_t ly = 0; ly < logH; ++ly) {
                for (int16_t lx = 0; lx < logW; ++lx) {
                    if (mirror) {
                        uint8_t count = mirror->mapToPhysical(
                            lx, ly, lz, physW, physH, physD,
                            physDests, physW);
                        lut_.setMapping(logIdx, physDests, count);
                    } else {
                        uint16_t physIdx = static_cast<uint16_t>(
                            lx + ly * physW + lz * physW * physH);
                        lut_.setMapping(logIdx, &physIdx, 1);
                    }
                    ++logIdx;
                }
            }
        }
        lut_.finalize();
    }

    // Render one frame: effects fill buffer sequentially, then
    // dynamic modifiers transform in order.
    void render(uint32_t frame) {
        RenderContext ctx{buffer_.pixels(), logDims_[0], logDims_[1], logDims_[2], frame};

        // Run effects in order (each writes to same buffer)
        for (uint8_t e = 0; e < effectCount_; ++e) {
            auto* eb = dynamic_cast<EffectBase*>(effects_[e]);
            if (eb) {
                eb->setContext(ctx);
            }
            effects_[e]->loop();
        }

        // Apply dynamic modifiers in order (each takes previous result)
        for (uint8_t m = 0; m < modifierCount_; ++m) {
            auto* rotate = dynamic_cast<RotateModifier*>(modifiers_[m]);
            if (rotate) {
                rotate->transformPixels(buffer_.pixels(), frame,
                                        logDims_[0], logDims_[1]);
            }
        }
    }

    Buffer& buffer() { return buffer_; }
    const Buffer& buffer() const { return buffer_; }
    MappingLUT& lut() { return lut_; }
    const MappingLUT& lut() const { return lut_; }
    LayoutGroup* layoutGroup() const { return fixture_; }
    uint8_t effectCount() const { return effectCount_; }
    uint8_t modifierCount() const { return modifierCount_; }
    MoonModule* effect(uint8_t i) const { return (i < effectCount_) ? effects_[i] : nullptr; }
    MoonModule* modifier(uint8_t i) const { return (i < modifierCount_) ? modifiers_[i] : nullptr; }

private:
    Buffer buffer_;
    MappingLUT lut_;
    MoonModule* effects_[MAX_EFFECTS] = {};
    uint8_t effectCount_ = 0;
    LayoutGroup* fixture_ = nullptr;
    MoonModule* modifiers_[MAX_MODIFIERS] = {};
    uint8_t modifierCount_ = 0;
    int16_t dims_[3] = {0, 0, 0};    // physical w, h, d from layout scan
    int16_t logDims_[3] = {0, 0, 0}; // logical w, h, d (after modifiers)
};

} // namespace mm::light
