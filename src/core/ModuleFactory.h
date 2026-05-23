#pragma once

#include "core/MoonModule.h"
#include "core/types.h"
#include "platform/platform.h"

#include <concepts>
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
    // Template registration: captures sizeof(T), role(), tags(), and dim()
    // automatically from a single stack-allocated probe instance. Registration
    // runs once at boot. docPath is the module's spec page relative to
    // docs/moonmodules/ (e.g. "light/effects/NoiseEffect.md"); the UI builds a
    // help link from it. It is a flash string literal — no per-instance RAM cost.
    // "" means no help link.
    //
    // dim is captured via `if constexpr` only when T declares `dimensions()`
    // (EffectBase and ModifierBase do; everything else doesn't). Modules without
    // it get dim=0 ("not applicable") and the UI skips their dimensional chip.
    template<typename T>
    static bool registerType(const char* typeName, const char* docPath = "") {
        if (!typeName) return false;
        if (!grow()) return false;
        T probe;
        uint8_t dim = 0;
        if constexpr (requires(const T& t) { { t.dimensions() } -> std::same_as<Dim>; }) {
            dim = static_cast<uint8_t>(probe.dimensions());
        }
        types_[count_++] = {typeName,
                            []() -> MoonModule* { return new T(); },
                            sizeof(T), probe.role(),
                            docPath ? docPath : "",
                            probe.tags() ? probe.tags() : "",
                            dim};
        return true;
    }

    // Non-template overload for callers that build the entry by hand (factory
    // functions that aren't a simple `new T()`, or test fixtures). Same shape as
    // the templated form's TypeEntry init — dim defaults to 0 because callers
    // who go through this path don't have a Dim to capture from.
    static bool registerType(const char* typeName, CreateModuleFn fn, size_t classSize = 0,
                             ModuleRole role = ModuleRole::Generic, const char* docPath = "",
                             const char* tags = "") {
        if (!typeName || !fn) return false;
        if (!grow()) return false;
        types_[count_++] = {typeName, fn, classSize, role, docPath ? docPath : "", tags ? tags : "", 0};
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
                    // Display name: typeName with the role-noun suffix stripped
                    // (NoiseEffect → Noise, MirrorModifier → Mirror, GridLayout → Grid,
                    // PreviewDriver → Preview). The role chip already conveys what kind of
                    // module it is; the card label shouldn't repeat it. typeName stays
                    // intact for persistence / lookup / replace.
                    mod->setName(displayNameFor(types_[i].name, types_[i].role));
                    if (types_[i].classSize > 0) mod->setClassSize(types_[i].classSize);
                }
                return mod;
            }
        }
        return nullptr;
    }

    // Strip the role-noun suffix from typeName if present, otherwise return as-is.
    // Returns a pointer into a small static buffer — caller copies it (setName does).
    // Role-based stripping:
    //   Effect   → strip "Effect"   (NoiseEffect → Noise)
    //   Modifier → strip "Modifier" (MirrorModifier → Mirror)
    //   Layout   → strip "Layout"   (GridLayout → Grid)
    //   Driver   → strip "Driver"   (PreviewDriver → Preview)
    //   Generic  → strip "Module"   (FilesystemModule → Filesystem)
    // Names without the suffix are returned unchanged (Layer, LayoutGroup, DriverGroup).
    static const char* displayNameFor(const char* typeName, ModuleRole role) {
        const char* suffix = nullptr;
        switch (role) {
            case ModuleRole::Effect:   suffix = "Effect";   break;
            case ModuleRole::Modifier: suffix = "Modifier"; break;
            case ModuleRole::Layout:   suffix = "Layout";   break;
            case ModuleRole::Driver:   suffix = "Driver";   break;
            case ModuleRole::Generic:  suffix = "Module";   break;
        }
        size_t typeLen = std::strlen(typeName);
        size_t suffixLen = std::strlen(suffix);
        if (typeLen <= suffixLen || std::strcmp(typeName + typeLen - suffixLen, suffix) != 0) {
            return typeName;  // Suffix not present — leave the name untouched.
        }
        // Copy the prefix into a static buffer (reused per create call; setName
        // copies it into the module's own name_ buffer immediately, so reuse is safe).
        static char displayBuf[16];
        size_t copyLen = typeLen - suffixLen;
        if (copyLen >= sizeof(displayBuf)) copyLen = sizeof(displayBuf) - 1;
        std::memcpy(displayBuf, typeName, copyLen);
        displayBuf[copyLen] = 0;
        return displayBuf;
    }

    static uint8_t typeCount() { return count_; }
    static const char* typeName(uint8_t i) { return (types_ && i < count_) ? types_[i].name : nullptr; }
    static ModuleRole typeRole(uint8_t i) { return (types_ && i < count_) ? types_[i].role : ModuleRole::Generic; }
    static const char* typeDocPath(uint8_t i) { return (types_ && i < count_) ? types_[i].docPath : ""; }
    static const char* typeTags(uint8_t i) { return (types_ && i < count_) ? types_[i].tags : ""; }
    // dim is 0 when the type's probe doesn't declare dimensions(), 1/2/3 when it
    // does (EffectBase/ModifierBase today). The UI uses this to derive 📏/🟦/🧊
    // alongside the role chip and origin emoji from tags().
    static uint8_t typeDim(uint8_t i) { return (types_ && i < count_) ? types_[i].dim : 0; }

private:
    struct TypeEntry {
        const char* name;
        CreateModuleFn create;
        size_t classSize;
        ModuleRole role;
        const char* docPath;
        const char* tags;
        uint8_t dim;  // 0 = N/A; 1/2/3 for types whose probe.dimensions() returns a Dim
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
