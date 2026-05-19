#include "core/Scheduler.h"
#include "light/GridLayout.h"
#include "light/RainbowEffect.h"
#include "light/ArtNetSendDriver.h"
#include "platform/platform.h"

#include <cstdio>

void mm_main(volatile bool& keepRunning, mm::lengthType gridW, mm::lengthType gridH) {
    mm::Scheduler scheduler;

    // Layout
    mm::LayoutGroup layoutGroup;
    layoutGroup.setName("LayoutGroup");

    mm::GridLayout grid;
    grid.setName("Grid");
    grid.width = gridW;
    grid.height = gridH;
    layoutGroup.addLayout(&grid);

    // Layer + Effect
    mm::Layer layer;
    layer.setName("Layer");
    layer.setLayoutGroup(&layoutGroup);
    layer.setChannelsPerLight(3);

    mm::RainbowEffect rainbow;
    rainbow.setName("Rainbow");
    layer.addEffect(&rainbow);

    // Driver Group + ArtNet
    mm::DriverGroup driverGroup;
    driverGroup.setName("DriverGroup");
    driverGroup.setLayer(&layer);

    mm::ArtNetSendDriver artnet;
    artnet.setName("ArtNet");
    driverGroup.addDriver(&artnet);

    // Register top-level modules with scheduler
    scheduler.addModule(&layoutGroup);
    scheduler.addModule(&layer);
    scheduler.addModule(&driverGroup);

    scheduler.setup();

    uint32_t lights = layoutGroup.totalLightCount();
    uint32_t bufBytes = lights * 3;
    std::printf("mmv3 running — grid %dx%d, %lu lights, buffer %lu bytes\n",
                grid.width, grid.height,
                static_cast<unsigned long>(lights),
                static_cast<unsigned long>(bufBytes));
    std::printf("ArtNet → %s\n", artnet.ip);

    size_t heap = mm::platform::freeHeap();
    if (heap > 0) {
        std::printf("Free heap: %u bytes\n", static_cast<unsigned>(heap));
    }
    std::fflush(stdout);

    uint32_t lastLog = mm::platform::millis();
    uint32_t frameCount = 0;

    while (keepRunning) {
        scheduler.tick();
        frameCount++;

        // Log every second
        uint32_t now = mm::platform::millis();
        if (now - lastLog >= 1000) {
            uint32_t fps = frameCount * 1000 / (now - lastLog);
            heap = mm::platform::freeHeap();
            if (heap > 0) {
                size_t maxBlock = mm::platform::maxAllocBlock();
                std::printf("FPS: %u  free: %u  maxBlock: %u\n",
                            static_cast<unsigned>(fps),
                            static_cast<unsigned>(heap),
                            static_cast<unsigned>(maxBlock));
            } else {
                std::printf("FPS: %u\n", static_cast<unsigned>(fps));
            }
            std::fflush(stdout);
            lastLog = now;
            frameCount = 0;
        }

        mm::platform::yield();
    }

    std::printf("\nShutting down.\n");
    scheduler.teardown();
}
