#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

enum class ControlType : uint8_t {
    Uint8,
    Uint16,
    Bool,
    Text
};

struct ControlDescriptor {
    void* ptr = nullptr;
    const char* name = nullptr;
    ControlType type = ControlType::Uint8;
    uint8_t min = 0;
    uint8_t max = 255;
};

class ControlList {
public:
    ~ControlList() { delete[] controls_; }

    void addUint8(const char* name, uint8_t& var, uint8_t min = 0, uint8_t max = 255) {
        grow();
        controls_[count_++] = {&var, name, ControlType::Uint8, min, max};
    }

    void addUint16(const char* name, uint16_t& var) {
        grow();
        controls_[count_++] = {&var, name, ControlType::Uint16, 0, 0};
    }

    // lengthType (int16_t) — same wire format as uint16, values are always positive for dimensions
    void addInt16(const char* name, int16_t& var) {
        grow();
        controls_[count_++] = {&var, name, ControlType::Uint16, 0, 0};
    }

    void addBool(const char* name, bool& var) {
        grow();
        controls_[count_++] = {&var, name, ControlType::Bool, 0, 1};
    }

    void addText(const char* name, char* var, uint8_t bufSize = 16) {
        grow();
        controls_[count_++] = {var, name, ControlType::Text, 0, bufSize};
    }

    void clear() { count_ = 0; }
    uint8_t count() const { return count_; }
    const ControlDescriptor& operator[](uint8_t i) const { return controls_[i]; }

private:
    ControlDescriptor* controls_ = nullptr;
    uint8_t count_ = 0;
    uint8_t capacity_ = 0;

    void grow() {
        if (count_ < capacity_) return;
        uint8_t newCap = capacity_ == 0 ? 4 : capacity_ * 2;
        auto* newArr = new ControlDescriptor[newCap];
        for (uint8_t i = 0; i < count_; i++) newArr[i] = controls_[i];
        delete[] controls_;
        controls_ = newArr;
        capacity_ = newCap;
    }
};

} // namespace mm
