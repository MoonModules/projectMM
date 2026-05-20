#include "core/Scheduler.h"
#include "light/GridLayout.h"
#include "light/RainbowEffect.h"
#include "light/NoiseEffect.h"
#include "light/MirrorModifier.h"
#include "light/ArtNetSendDriver.h"
#include "light/PreviewDriver.h"
#include "core/HttpServerModule.h"
#include "platform/platform.h"

#include <cstdio>

void mm_main(volatile bool& keepRunning, mm::lengthType gridW, mm::lengthType gridH, uint16_t httpPort) {
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

    mm::NoiseEffect noise;
    noise.setName("Noise");
    layer.addEffect(&noise);

    // Modifier
    mm::MirrorModifier mirror;
    mirror.setName("Mirror");
    layer.addModifier(&mirror);

    // Driver Group + ArtNet
    mm::DriverGroup driverGroup;
    driverGroup.setName("DriverGroup");
    driverGroup.setLayer(&layer);

    mm::ArtNetSendDriver artnet;
    artnet.setName("ArtNet");
    driverGroup.addDriver(&artnet);

    // Preview driver (WebSocket binary frames)
    mm::PreviewFrame previewFrame;
    mm::PreviewDriver preview;
    preview.setName("Preview");
    preview.width = gridW;
    preview.height = gridH;
    preview.setPreviewFrame(&previewFrame);
    driverGroup.addDriver(&preview);

    // HTTP Server + WebSocket
    mm::HttpServerModule httpServer;
    httpServer.setName("HttpServer");
    httpServer.port = httpPort;
    httpServer.setScheduler(&scheduler);
    httpServer.setPreviewFrame(&previewFrame);

    // Register top-level modules with scheduler
    scheduler.addModule(&layoutGroup);
    scheduler.addModule(&layer);
    scheduler.addModule(&driverGroup);
    scheduler.addModule(&httpServer);

    scheduler.setup();

    uint32_t lights = layoutGroup.totalLightCount();
    uint32_t bufBytes = lights * 3;
    std::printf("mmv3 running — grid %dx%d, %lu lights, buffer %lu bytes\n",
                grid.width, grid.height,
                static_cast<unsigned long>(lights),
                static_cast<unsigned long>(bufBytes));
    std::printf("ArtNet → %s\n", artnet.ip);
    std::printf("HTTP server → http://localhost:%u\n", httpServer.port);

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
