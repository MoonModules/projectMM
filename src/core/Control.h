#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

enum class ControlType : uint8_t {
    Uint8,
    Uint16,
    Bool,
    Text,
    ReadOnly,   // display-only text (ptr → char buffer)
    Select,     // dropdown (ptr → uint8_t index, aux → options array pointer)
    Progress    // bar with value/total (ptr → uint32_t value, aux = total)
};

struct ControlDescriptor {
    void* ptr = nullptr;
    const char* name = nullptr;
    uintptr_t aux = 0;      // Progress: total capacity. Select: pointer to options array.
    ControlType type = ControlType::Uint8;
    uint8_t min = 0;
    uint8_t max = 255;
    bool hidden = false;    // UI visibility flag. Set via ControlList::setHidden() after addX().
                            // Persistence ignores this — hidden controls are still saved/loaded
                            // so toggling visibility doesn't lose state.
};

class ControlList {
public:
    ~ControlList() { delete[] controls_; }

    ControlList() = default;
    ControlList(const ControlList&) = delete;
    ControlList& operator=(const ControlList&) = delete;
    ControlList(ControlList&&) = delete;
    ControlList& operator=(ControlList&&) = delete;

    void addUint8(const char* name, uint8_t& var, uint8_t min = 0, uint8_t max = 255) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Uint8, min, max};
    }

    void addUint16(const char* name, uint16_t& var) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Uint16, 0, 0};
    }

    // lengthType (int16_t) — same wire format as uint16, values are always positive for dimensions
    void addInt16(const char* name, int16_t& var) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Uint16, 0, 0};
    }

    void addBool(const char* name, bool& var) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Bool, 0, 1};
    }

    void addText(const char* name, char* var, uint8_t bufSize = 16) {
        grow();
        controls_[count_++] = {var, name, 0, ControlType::Text, 0, bufSize};
    }

    void addReadOnly(const char* name, char* var, uint8_t bufSize = 32) {
        grow();
        controls_[count_++] = {var, name, 0, ControlType::ReadOnly, 0, bufSize};
    }

    void addSelect(const char* name, uint8_t& var, const char* const* options, uint8_t optionCount) {
        grow();
        controls_[count_++] = {&var, name, reinterpret_cast<uintptr_t>(options), ControlType::Select, 0, optionCount};
    }

    void addProgress(const char* name, uint32_t& var, uint32_t total) {
        grow();
        controls_[count_++] = {&var, name, total, ControlType::Progress, 0, 0};
    }

    void clear() { count_ = 0; }
    uint8_t count() const { return count_; }
    const ControlDescriptor& operator[](uint8_t i) const { return controls_[i]; }

    // Flip the hidden flag on a previously-added control. Typical use: call addX() then
    // setHidden(count() - 1, condition). Hidden controls are not rendered in the UI but
    // remain bound for persistence — toggling visibility doesn't lose state.
    void setHidden(uint8_t i, bool hidden) {
        if (i < count_) controls_[i].hidden = hidden;
    }

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
