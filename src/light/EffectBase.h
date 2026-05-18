#pragma once

#include "core/MoonModule.h"
#include "light/RenderContext.h"

namespace mm::light {

// Light-domain MoonModule subclass for effects.
// Adds rendering context (buffer, dimensions, frame number).
class EffectBase : public MoonModule {
public:
    void setContext(const RenderContext& ctx) { ctx_ = ctx; }

protected:
    const RenderContext& context() const { return ctx_; }

    // Convenience accessors for use in loop()
    std::span<RGB> pixels() { return ctx_.pixels; }
    int16_t width() const { return ctx_.width; }
    int16_t height() const { return ctx_.height; }
    int16_t depth() const { return ctx_.depth; }
    uint32_t frame() const { return ctx_.frame; }

private:
    RenderContext ctx_;
};

} // namespace mm::light
