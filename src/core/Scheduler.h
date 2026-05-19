#pragma once

#include "core/MoonModule.h"
#include "platform/platform.h"

#include <array>

namespace mm {

class Scheduler {
public:
    void addModule(MoonModule* mod) {
        if (moduleCount_ >= modules_.size()) return;
        modules_[moduleCount_++] = mod;
    }

    void setup() {
        startTime_ = platform::millis();
        for (uint8_t i = 0; i < moduleCount_; i++) {
            modules_[i]->setup();
        }
        for (uint8_t i = 0; i < moduleCount_; i++) {
            modules_[i]->onBuildControls();
        }
        for (uint8_t i = 0; i < moduleCount_; i++) {
            modules_[i]->onAllocateMemory();
        }
        lastLoop20ms_ = platform::millis();
        lastLoop1s_ = platform::millis();
    }

    void tick() {
        uint32_t now = platform::millis();

        for (uint8_t i = 0; i < moduleCount_; i++) {
            modules_[i]->loop();
        }

        if (now - lastLoop20ms_ >= 20) {
            lastLoop20ms_ = now;
            for (uint8_t i = 0; i < moduleCount_; i++) {
                modules_[i]->loop20ms();
            }
        }

        if (now - lastLoop1s_ >= 1000) {
            lastLoop1s_ = now;
            for (uint8_t i = 0; i < moduleCount_; i++) {
                modules_[i]->loop1s();
            }
        }
    }

    void teardown() {
        for (uint8_t i = moduleCount_; i > 0; i--) {
            modules_[i - 1]->teardown();
        }
    }

    uint32_t elapsed() const {
        return platform::millis() - startTime_;
    }

private:
    std::array<MoonModule*, 32> modules_{};
    uint8_t moduleCount_ = 0;
    uint32_t startTime_ = 0;
    uint32_t lastLoop20ms_ = 0;
    uint32_t lastLoop1s_ = 0;
};

} // namespace mm
