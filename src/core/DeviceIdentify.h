#pragma once

#include <cstdint>

// The device-kind enum + its wire labels, shared by DevicesModule, the interop
// plugins (core/DevicePlugin.h), persistence, and the UI list. The matching plugin
// classifies a device straight from its UDP presence packet (the 44-byte WledPacket
// header, with a projectMM marker distinguishing a peer from a plain WLED), so the
// classification logic lives with each plugin; this header carries only the small
// shared vocabulary so the enum doesn't pull in the module.

namespace mm {

// What a discovered device is.
enum class DevType : uint8_t { Generic = 0, ProjectMM = 1, Wled = 2, Hue = 3 };

inline const char* devTypeStr(DevType t) {
    switch (t) {
        case DevType::ProjectMM: return "projectMM";
        case DevType::Wled:      return "WLED";
        case DevType::Hue:       return "Hue bridge";
        case DevType::Generic:   return "generic";
    }
    return "generic";
}

}  // namespace mm
