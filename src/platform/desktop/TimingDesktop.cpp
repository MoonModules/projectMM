#include "platform/Timing.h"
#include <chrono>

namespace mm::platform {

static auto start = std::chrono::steady_clock::now();

uint32_t millis() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
}

uint64_t micros() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - start).count());
}

} // namespace mm::platform
