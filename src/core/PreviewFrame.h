#pragma once

#include "core/types.h"
#include <cstdint>
#include <cstddef>

namespace mm {

// Zero-copy preview: points to the existing output buffer, no allocation.
// PreviewDriver updates the pointer each frame. HttpServerModule reads it.
// Single-threaded scheduler — no lock needed.
struct PreviewFrame {
    const uint8_t* data = nullptr;  // points to existing buffer (no ownership)
    size_t dataLen = 0;
    lengthType width = 0;
    lengthType height = 0;
    lengthType depth = 0;
    uint8_t fps = 20;
    bool ready = false;
};

} // namespace mm
