#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

enum class ControlType : uint8_t {
    Uint16,     // slider: uint16_t value with min/max
    Bool,       // toggle: true/false
    Text,       // text input: char[64] (IP addresses, names, etc.)
};

struct Control {
    char name[24] = {};
    ControlType type = ControlType::Uint16;
    union {
        struct { uint16_t value, min, max; } u16;
        struct { bool value; } b;
        struct { char value[64]; } text;
    };

    Control() : u16{0, 0, 0} {}

    void setName(const char* n) {
        std::strncpy(name, n, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }
};

} // namespace mm
