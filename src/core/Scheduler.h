#pragma once

#include "core/MoonModule.h"
#include "platform/platform.h"

#include <array>

namespace mm {

class Scheduler {
public:
    void addModule(MoonModule* mod) {
        if (!mod || moduleCount_ >= modules_.size()) return;
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
        lastTimingUpdate_ = platform::millis();
    }

    void tick() {
        uint32_t now = platform::millis();
        uint32_t tickStart = platform::micros();

        // loop() — every tick, timed per module (skip disabled)
        for (uint8_t i = 0; i < moduleCount_; i++) {
            if (!modules_[i]->enabled()) continue;
            uint32_t modStart = platform::micros();
            modules_[i]->loop();
            modules_[i]->addAccumUs(platform::micros() - modStart);
        }

        // loop20ms — timed per module too
        if (now - lastLoop20ms_ >= 20) {
            lastLoop20ms_ = now;
            for (uint8_t i = 0; i < moduleCount_; i++) {
                if (!modules_[i]->enabled()) continue;
                uint32_t modStart = platform::micros();
                modules_[i]->loop20ms();
                modules_[i]->addAccumUs(platform::micros() - modStart);
            }
        }

        // loop1s — timed per module too
        if (now - lastLoop1s_ >= 1000) {
            lastLoop1s_ = now;
            for (uint8_t i = 0; i < moduleCount_; i++) {
                if (!modules_[i]->enabled()) continue;
                uint32_t modStart = platform::micros();
                modules_[i]->loop1s();
                modules_[i]->addAccumUs(platform::micros() - modStart);
            }
        }

        tickAccumUs_ += platform::micros() - tickStart;
        frameCount_++;

        // Every 1 second: compute averages, recurse into children
        if (now - lastTimingUpdate_ >= 1000) {
            tickTimeUs_ = frameCount_ > 0 ? tickAccumUs_ / frameCount_ : 0;

            for (uint8_t i = 0; i < moduleCount_; i++) {
                modules_[i]->publishTiming(frameCount_);
            }

            tickAccumUs_ = 0;
            frameCount_ = 0;
            lastTimingUpdate_ = now;
        }
    }

    void teardown() {
        // Two passes: tear down all modules first (so a module's teardown can still safely
        // observe sibling modules' state), then delete the trees. Otherwise the reverse-order
        // teardown-then-delete pattern would leave a module's teardown looking at already-freed
        // siblings — relevant for any cross-module cleanup work.
        for (uint8_t i = moduleCount_; i > 0; i--) {
            modules_[i - 1]->teardown();
        }
        for (uint8_t i = moduleCount_; i > 0; i--) {
            deleteTree(modules_[i - 1]);
        }
        moduleCount_ = 0;
    }

    uint32_t elapsed() const {
        return platform::millis() - startTime_;
    }

    void rebuild() {
        for (uint8_t i = 0; i < moduleCount_; i++) {
            modules_[i]->onAllocateMemory();
        }
    }

    uint32_t tickTimeUs() const { return tickTimeUs_; }
    uint32_t fps() const { return tickTimeUs_ > 0 ? 1000000 / tickTimeUs_ : 0; }
    uint8_t moduleCount() const { return moduleCount_; }
    MoonModule* module(uint8_t i) const { return i < moduleCount_ ? modules_[i] : nullptr; }

    static void deleteTree(MoonModule* mod) {
        if (!mod) return;
        for (uint8_t i = 0; i < mod->childCount(); i++) {
            deleteTree(mod->child(i));
        }
        delete mod;
    }

private:
    std::array<MoonModule*, 32> modules_{};
    uint8_t moduleCount_ = 0;
    uint32_t startTime_ = 0;
    uint32_t lastLoop20ms_ = 0;
    uint32_t lastLoop1s_ = 0;
    uint32_t tickTimeUs_ = 0;
    uint32_t tickAccumUs_ = 0;
    uint32_t frameCount_ = 0;        // frames in current 1-second window (for averaging)
    uint32_t lastTimingUpdate_ = 0;   // 1-second window start
};

} // namespace mm
