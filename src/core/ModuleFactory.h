#pragma once

#include "core/MoonModule.h"
#include "platform/platform.h"

#include <cstring>

namespace mm {

using CreateModuleFn = MoonModule*(*)();

// Static registry mapping type names → factory functions. Grow-on-demand storage
// (capacity doubles from 0 → 4 → 8 → 16 → …) lives on the heap, preferring PSRAM
// on ESP32 via platform::alloc. Eliminates the fixed-cap waste of the previous
// MAX_TYPES=32 static array (~1 KB of internal RAM held empty when fewer types
// were registered). Mirrors the dynamic-over-fixed pattern used by
// MoonModule::children_ and ControlList::controls_.
class ModuleFactory {
public:
    // Template registration: captures sizeof(T) and role() automatically. Role is
    // discovered via a stack-allocated probe instance at registration time — no per-type
    // boilerplate, no member-pointer storage. Registration runs once at boot.
    template<typename T>
    static bool registerType(const char* typeName) {
        T probe;
        ModuleRole role = probe.role();
        return registerType(typeName, []() -> MoonModule* { return new T(); }, sizeof(T), role);
    }

    static bool registerType(const char* typeName, CreateModuleFn fn, size_t classSize = 0,
                             ModuleRole role = ModuleRole::Generic) {
        if (!typeName || !fn) return false;
        if (!grow()) return false;
        types_[count_++] = {typeName, fn, classSize, role};
        return true;
    }

    static MoonModule* create(const char* typeName) {
        if (!typeName || !types_) return nullptr;
        for (uint8_t i = 0; i < count_; i++) {
            if (std::strcmp(types_[i].name, typeName) == 0) {
                auto* mod = types_[i].create();
                if (mod) {
                    // typeName_ stores a pointer (no copy), so we hand the module the registry's
                    // canonical string literal — not the caller's `typeName` parameter, which may
                    // be a stack buffer (e.g. persistence reading JSON). The registry pointer is
                    // a flash literal from registerType<T>("…"), valid for the program lifetime.
                    mod->setTypeName(types_[i].name);
                    mod->setName(types_[i].name);
                    if (types_[i].classSize > 0) mod->setClassSize(types_[i].classSize);
                }
                return mod;
            }
        }
        return nullptr;
    }

    static uint8_t typeCount() { return count_; }
    static const char* typeName(uint8_t i) { return (types_ && i < count_) ? types_[i].name : nullptr; }
    static ModuleRole typeRole(uint8_t i) { return (types_ && i < count_) ? types_[i].role : ModuleRole::Generic; }

private:
    struct TypeEntry {
        const char* name;
        CreateModuleFn create;
        size_t classSize;
        ModuleRole role;
    };

    static inline TypeEntry* types_ = nullptr;
    static inline uint8_t count_ = 0;
    static inline uint8_t capacity_ = 0;

    // Ensure there's room for one more entry. Doubles capacity 0 → 4 → 8 → 16 → ….
    // Returns false on allocation failure (out of memory; registration is rejected).
    static bool grow() {
        if (count_ < capacity_) return true;
        uint8_t newCap = capacity_ == 0 ? 4 : (capacity_ < 128 ? capacity_ * 2 : 255);
        if (newCap == capacity_) return false;  // saturated at uint8_t ceiling
        auto* newArr = static_cast<TypeEntry*>(platform::alloc(sizeof(TypeEntry) * newCap));
        if (!newArr) return false;
        for (uint8_t i = 0; i < count_; i++) newArr[i] = types_[i];
        if (types_) platform::free(types_);
        types_ = newArr;
        capacity_ = newCap;
        return true;
    }
};

} // namespace mm
