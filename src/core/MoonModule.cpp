#include "core/MoonModule.h"
#include <algorithm>
#include <cstring>

namespace mm {

uint8_t MoonModule::addControl(const char* name, uint16_t value,
                                uint16_t min, uint16_t max) {
    if (controlCount_ >= MAX_CONTROLS) return MAX_CONTROLS;
    auto& c = controls_[controlCount_];
    c.setName(name);
    c.type = ControlType::Uint16;
    c.u16.value = std::clamp(value, min, max);
    c.u16.min = min;
    c.u16.max = max;
    return controlCount_++;
}

uint8_t MoonModule::addControl(const char* name, bool value) {
    if (controlCount_ >= MAX_CONTROLS) return MAX_CONTROLS;
    auto& c = controls_[controlCount_];
    c.setName(name);
    c.type = ControlType::Bool;
    c.b.value = value;
    return controlCount_++;
}

uint8_t MoonModule::addControl(const char* name, const char* value) {
    if (controlCount_ >= MAX_CONTROLS) return MAX_CONTROLS;
    auto& c = controls_[controlCount_];
    c.setName(name);
    c.type = ControlType::Text;
    std::strncpy(c.text.value, value, sizeof(c.text.value) - 1);
    c.text.value[sizeof(c.text.value) - 1] = '\0';
    return controlCount_++;
}

Control* MoonModule::control(uint8_t index) {
    if (index >= controlCount_) return nullptr;
    return &controls_[index];
}

const Control* MoonModule::control(uint8_t index) const {
    if (index >= controlCount_) return nullptr;
    return &controls_[index];
}

Control* MoonModule::controlByName(const char* name) {
    for (uint8_t i = 0; i < controlCount_; ++i) {
        if (std::strcmp(controls_[i].name, name) == 0) {
            return &controls_[i];
        }
    }
    return nullptr;
}

void MoonModule::setControl(uint8_t index, uint16_t value) {
    if (index >= controlCount_) return;
    auto& c = controls_[index];
    if (c.type != ControlType::Uint16) return;
    uint16_t clamped = std::clamp(value, c.u16.min, c.u16.max);
    if (c.u16.value != clamped) {
        c.u16.value = clamped;
        onChange(index);
    }
}

void MoonModule::setControl(uint8_t index, bool value) {
    if (index >= controlCount_) return;
    auto& c = controls_[index];
    if (c.type != ControlType::Bool) return;
    if (c.b.value != value) {
        c.b.value = value;
        onChange(index);
    }
}

void MoonModule::setControl(uint8_t index, const char* value) {
    if (index >= controlCount_) return;
    auto& c = controls_[index];
    if (c.type != ControlType::Text) return;
    if (std::strcmp(c.text.value, value) != 0) {
        std::strncpy(c.text.value, value, sizeof(c.text.value) - 1);
        c.text.value[sizeof(c.text.value) - 1] = '\0';
        onChange(index);
    }
}

} // namespace mm
