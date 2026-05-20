#pragma once

#include "core/MoonModule.h"
#include <cstring>
#include <array>

namespace mm {

using CreateModuleFn = MoonModule*(*)();

class ModuleFactory {
public:
    // Template registration: captures sizeof(T) automatically
    template<typename T>
    static bool registerType(const char* typeName) {
        return registerType(typeName, []() -> MoonModule* { return new T(); }, sizeof(T));
    }

    static bool registerType(const char* typeName, CreateModuleFn fn, size_t classSize = 0) {
        if (!typeName || !fn) return false;
        if (count_ >= MAX_TYPES) return false;
        types_[count_] = {typeName, fn, classSize};
        count_++;
        return true;
    }

    static MoonModule* create(const char* typeName) {
        if (!typeName) return nullptr;
        for (uint8_t i = 0; i < count_; i++) {
            if (std::strcmp(types_[i].name, typeName) == 0) {
                auto* mod = types_[i].create();
                if (mod) {
                    mod->setName(typeName);
                    if (types_[i].classSize > 0) mod->setClassSize(types_[i].classSize);
                }
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
        size_t classSize;
    };

    static inline std::array<TypeEntry, MAX_TYPES> types_{};
    static inline uint8_t count_ = 0;
};

} // namespace mm
