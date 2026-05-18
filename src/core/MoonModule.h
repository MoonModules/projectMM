#pragma once

#include "core/Control.h"
#include <cstdint>
#include <cstring>

namespace mm {

class MoonModule {
public:
    virtual ~MoonModule() = default;
    virtual const char* name() const = 0;

    // Lifecycle — called by the scheduler
    virtual void setup() {}
    virtual void loop() {}
    virtual void teardown() {}

    // Controls — override to declare controls at init time
    virtual void addControls() {}
    virtual void onChange(uint8_t index) { (void)index; }

    // Add controls (returns index, or MAX_CONTROLS on failure)
    uint8_t addControl(const char* name, uint16_t value,
                       uint16_t min, uint16_t max);
    uint8_t addControl(const char* name, bool value);
    uint8_t addControl(const char* name, const char* value);

    // Access controls
    Control* control(uint8_t index);
    const Control* control(uint8_t index) const;
    Control* controlByName(const char* name);
    uint8_t controlCount() const { return controlCount_; }

    // Set control values (calls onChange if value changed)
    void setControl(uint8_t index, uint16_t value);
    void setControl(uint8_t index, bool value);
    void setControl(uint8_t index, const char* value);

    static constexpr uint8_t MAX_CONTROLS = 16;

    // Dirty flag — set by onChange, cleared by consumer
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // Non-copyable
    MoonModule(const MoonModule&) = delete;
    MoonModule& operator=(const MoonModule&) = delete;

protected:
    MoonModule() = default;
    void markDirty() { dirty_ = true; }

private:
    Control controls_[MAX_CONTROLS] = {};
    uint8_t controlCount_ = 0;
    bool dirty_ = false;
};

} // namespace mm
