#pragma once

#include "core/Control.h"

namespace mm {

enum class ModuleRole : uint8_t { Generic, Effect, Modifier, Driver, Layout };

class MoonModule {
public:
    virtual ~MoonModule() { delete[] children_; }

    // Default lifecycle propagates to children. Override to add container-specific logic.
    virtual void setup() { for (uint8_t i = 0; i < childCount_; i++) children_[i]->setup(); }
    virtual void loop() {}
    virtual void loop20ms() {}
    virtual void loop1s() {}
    virtual void teardown() { for (uint8_t i = childCount_; i > 0; i--) children_[i-1]->teardown(); }
    virtual void onBuildControls() { for (uint8_t i = 0; i < childCount_; i++) children_[i]->onBuildControls(); }
    virtual void onAllocateMemory() { for (uint8_t i = 0; i < childCount_; i++) children_[i]->onAllocateMemory(); }

    const char* name() const { return name_; }
    void setName(const char* n) { name_ = n; }

    MoonModule* parent() const { return parent_; }
    void setParent(MoonModule* p) { parent_ = p; }

    ControlList& controls() { return controls_; }
    const ControlList& controls() const { return controls_; }

    // Role for type identification (no RTTI needed)
    virtual ModuleRole role() const { return ModuleRole::Generic; }

    // Generic children — grows on demand, only allocates during setup
    bool addChild(MoonModule* child) {
        if (!child) return false;
        if (childCount_ == childCapacity_) {
            uint8_t newCap = childCapacity_ == 0 ? 4 : childCapacity_ * 2;
            auto** newArr = new MoonModule*[newCap];
            for (uint8_t i = 0; i < childCount_; i++) newArr[i] = children_[i];
            delete[] children_;
            children_ = newArr;
            childCapacity_ = newCap;
        }
        child->setParent(this);
        children_[childCount_++] = child;
        return true;
    }

    bool removeChild(MoonModule* child) {
        for (uint8_t i = 0; i < childCount_; i++) {
            if (children_[i] == child) {
                for (uint8_t j = i; j + 1 < childCount_; j++) children_[j] = children_[j + 1];
                childCount_--;
                return true;
            }
        }
        return false;
    }

    uint8_t childCount() const { return childCount_; }
    MoonModule* child(uint8_t i) const { return i < childCount_ ? children_[i] : nullptr; }

    // Per-module timing: parents time children, Scheduler times top-level
    uint32_t loopTimeUs() const { return loopTimeUs_; }
    void addAccumUs(uint32_t us) { accumUs_ += us; }

    // Called by Scheduler every ~1 second. Recurses into children.
    void publishTiming(uint32_t frameCount) {
        loopTimeUs_ = frameCount > 0 ? accumUs_ / frameCount : 0;
        accumUs_ = 0;
        for (uint8_t i = 0; i < childCount_; i++) {
            children_[i]->publishTiming(frameCount);
        }
    }

protected:
    ControlList controls_;

private:
    const char* name_ = nullptr;
    MoonModule* parent_ = nullptr;
    MoonModule** children_ = nullptr;
    uint8_t childCount_ = 0;
    uint8_t childCapacity_ = 0;
    uint32_t loopTimeUs_ = 0;
    uint32_t accumUs_ = 0;
};

} // namespace mm
