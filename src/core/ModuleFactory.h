#pragma once

#include "core/MoonModule.h"
#include <cstring>
#include <array>

namespace mm {

using CreateModuleFn = MoonModule*(*)();

class ModuleFactory {
public:
    static bool registerType(const char* typeName, CreateModuleFn fn) {
        if (count_ >= MAX_TYPES) return false;
        types_[count_] = {typeName, fn};
        count_++;
        return true;
    }

    static MoonModule* create(const char* typeName) {
        for (uint8_t i = 0; i < count_; i++) {
            if (std::strcmp(types_[i].name, typeName) == 0) {
                auto* mod = types_[i].create();
                if (mod) mod->setName(typeName);
                return mod;
            }
        }
        return nullptr;
    }

    static uint8_t typeCount() { return count_; }
    static const char* typeName(uint8_t i) { return i < count_ ? types_[i].name : nullptr; }

private:
    static constexpr uint8_t MAX_TYPES = 32;

    struct TypeEntry {
        const char* name;
        CreateModuleFn create;
    };

    static inline std::array<TypeEntry, MAX_TYPES> types_{};
    static inline uint8_t count_ = 0;
};

} // namespace mm
