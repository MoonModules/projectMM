#pragma once

#include <cstdint>
#include <cstring>
#include <array>

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

template<size_t Capacity = 8>
class ControlList {
public:
    void addUint8(const char* name, uint8_t& var, uint8_t min = 0, uint8_t max = 255) {
        if (count_ >= Capacity) return;
        controls_[count_++] = {&var, name, ControlType::Uint8, min, max};
    }

    void addUint16(const char* name, uint16_t& var) {
        if (count_ >= Capacity) return;
        controls_[count_++] = {&var, name, ControlType::Uint16, 0, 0};
    }

    void addBool(const char* name, bool& var) {
        if (count_ >= Capacity) return;
        controls_[count_++] = {&var, name, ControlType::Bool, 0, 1};
    }

    void addText(const char* name, char* var) {
        if (count_ >= Capacity) return;
        controls_[count_++] = {var, name, ControlType::Text, 0, 0};
    }

    void clear() { count_ = 0; }
    uint8_t count() const { return count_; }
    const ControlDescriptor& operator[](uint8_t i) const { return controls_[i]; }

private:
    std::array<ControlDescriptor, Capacity> controls_{};
    uint8_t count_ = 0;
};

} // namespace mm
