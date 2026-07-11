#pragma once

#include <atomic>
#include <cstdint>

namespace mm {

inline std::atomic<bool> g_otaInFlight{false};
inline char     g_otaStatus[64] = "idle";
inline uint32_t g_otaBytesRead  = 0;
inline uint32_t g_otaBytesTotal = 0;

inline bool otaInFlight() {
    return g_otaInFlight.load(std::memory_order_acquire);
}

inline bool otaTryStart() {
    bool expected = false;
    return g_otaInFlight.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
}

inline void otaFinish() {
    g_otaInFlight.store(false, std::memory_order_release);
}

} // namespace mm
