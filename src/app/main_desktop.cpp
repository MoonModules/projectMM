#include "core/AppState.h"
#include "core/Scheduler.h"
#include "core/modules/HttpServerModule.h"
#include "light/DriverGroup.h"
#include "light/Layer.h"
#include "light/LayoutGroup.h"
#include "light/modules/drivers/ArtNetSendDriver.h"
#include "light/modules/effects/NoiseEffect.h"
#include "light/modules/effects/RainbowEffect.h"
#include "light/modules/layouts/GridLayout.h"
#include "light/modules/modifiers/MirrorModifier.h"
#include "light/modules/modifiers/RotateModifier.h"
#include "platform/Timing.h"
#include <cstdio>
#include <thread>

using namespace mm;
using namespace mm::light;

// GridLayout adapter for LayoutGroup
class GridAdapter : public GridLayout, public LayoutBase {
public:
    size_t pixelCount() const override { return GridLayout::pixelCount(); }
    void forEachCoord(CoordCallback cb, void* ctx) const override {
        GridLayout::forEachCoord(cb, ctx);
    }
};

int main() {
    std::printf("projectMM v3 starting...\n");

    // --- Layout ---
    GridAdapter gridLayout;

    LayoutGroup layoutGroup;
    layoutGroup.addLayout(&gridLayout);

    // --- Effects ---
    RainbowEffect rainbowEffect;
    NoiseEffect noiseEffect;
    MoonModule* availableEffects[] = { &rainbowEffect, &noiseEffect };

    // --- Modifiers ---
    MirrorModifier mirrorModifier;
    RotateModifier rotateModifier;
    MoonModule* availableModifiers[] = { &mirrorModifier, &rotateModifier };

    // --- Layer ---
    Layer layer;
    layer.setLayoutGroup(&layoutGroup);
    layer.setEffect(&rainbowEffect);

    // --- Drivers ---
    ArtNetSendDriver artnetDriver;

    DriverGroup driverGroup;
    driverGroup.addDriver(&artnetDriver);
    driverGroup.setLayers(&layer, 1);

    // --- HTTP Server ---
    HttpServerModule httpServer;
    httpServer.setUiPath("src/ui");

    MoonModule* availableLayouts[] = { &gridLayout };
    MoonModule* availableDrivers[] = { &artnetDriver };

    AppState appState;
    appState.layoutGroup = &layoutGroup;
    appState.layers = &layer;
    appState.layerCount = 1;
    appState.driverGroup = &driverGroup;
    appState.availableEffects = availableEffects;
    appState.effectCount = 2;
    appState.availableLayouts = availableLayouts;
    appState.layoutCount = 1;
    appState.availableDrivers = availableDrivers;
    appState.driverCount = 1;
    appState.availableModifiers = availableModifiers;
    appState.modifierCount = 2;
    httpServer.setAppState(&appState);

    // --- Scheduler ---
    Scheduler scheduler;
    scheduler.add(&gridLayout);
    scheduler.add(&rainbowEffect);
    scheduler.add(&noiseEffect);
    scheduler.add(&mirrorModifier);
    scheduler.add(&rotateModifier);
    scheduler.add(&artnetDriver);
    scheduler.add(&driverGroup);
    scheduler.add(&httpServer);

    appState.scheduler = &scheduler;

    scheduler.setup();

    // Rebuild LUT after setup (controls are now initialized)
    layer.rebuildLUT();
    driverGroup.allocateOutput(layoutGroup.totalPixelCount());

    std::printf("Running: %zu pixels, HTTP on port %u\n",
                layoutGroup.totalPixelCount(),
                httpServer.control(0)->u16.value);

    // --- Main loop ---
    constexpr uint32_t TARGET_FPS = 60;
    constexpr uint32_t FRAME_MS = 1000 / TARGET_FPS;

    while (true) {
        uint32_t frameStart = platform::millis();

        // Check if layout or modifiers changed — rebuild LUT and resize output
        if (gridLayout.dirty() || mirrorModifier.dirty() || rotateModifier.dirty()) {
            gridLayout.clearDirty();
            mirrorModifier.clearDirty();
            rotateModifier.clearDirty();
            layer.rebuildLUT();
            driverGroup.allocateOutput(layoutGroup.totalPixelCount());
        }

        // Render layers
        layer.render(scheduler.frame());

        // Scheduler loop (drives driverGroup which does blendMap + drivers,
        // and httpServer which polls for connections)
        scheduler.loop();

        // Frame timing
        uint32_t elapsed = platform::millis() - frameStart;
        if (elapsed < FRAME_MS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(FRAME_MS - elapsed));
        }
    }

    scheduler.teardown();
    return 0;
}
