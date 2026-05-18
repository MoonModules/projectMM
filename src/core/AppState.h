#pragma once

#include <cstddef>

namespace mm {

class MoonModule;
class Scheduler;

namespace light {
    class LayoutGroup;
    class Layer;
    class DriverGroup;
}

// Provides the HTTP server (and other system modules) with access to
// the application's MoonModule tree. Set by the app entry point.
struct AppState {
    light::LayoutGroup* layoutGroup = nullptr;
    light::Layer* layers = nullptr;
    size_t layerCount = 0;
    light::DriverGroup* driverGroup = nullptr;
    Scheduler* scheduler = nullptr;

    // Available effects/layouts that can be switched to
    MoonModule** availableEffects = nullptr;
    size_t effectCount = 0;
    MoonModule** availableLayouts = nullptr;
    size_t layoutCount = 0;
    MoonModule** availableDrivers = nullptr;
    size_t driverCount = 0;
    MoonModule** availableModifiers = nullptr;
    size_t modifierCount = 0;
};

} // namespace mm
