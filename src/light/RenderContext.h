#pragma once

#include "light/Pixel.h"
#include <cstdint>
#include <span>

namespace mm::light {

struct RenderContext {
    std::span<RGB> pixels;
    int16_t width = 0;
    int16_t height = 0;
    int16_t depth = 0;
    uint32_t frame = 0;
};

} // namespace mm::light
