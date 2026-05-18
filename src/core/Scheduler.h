#pragma once

#include "core/MoonModule.h"
#include "platform/Timing.h"
#include <cstdint>

namespace mm {

class Scheduler {
public:
    static constexpr uint8_t MAX_MODULES = 32;

    void add(MoonModule* mod) {
        if (count_ < MAX_MODULES) {
            modules_[count_++] = mod;
        }
    }

    void setup() {
        startTime_ = platform::millis();
        frame_ = 0;
        for (uint8_t i = 0; i < count_; ++i) {
            modules_[i]->addControls();
            modules_[i]->setup();
        }
    }

    void loop() {
        for (uint8_t i = 0; i < count_; ++i) {
            modules_[i]->loop();
        }
        ++frame_;
    }

    void teardown() {
        for (uint8_t i = 0; i < count_; ++i) {
            modules_[i]->teardown();
        }
    }

    uint32_t frame() const { return frame_; }
    uint32_t elapsed() const { return platform::millis() - startTime_; }
    uint8_t count() const { return count_; }

private:
    MoonModule* modules_[MAX_MODULES] = {};
    uint8_t count_ = 0;
    uint32_t frame_ = 0;
    uint32_t startTime_ = 0;
};

} // namespace mm
