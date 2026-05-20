#include "core/Scheduler.h"
#include "light/GridLayout.h"
#include "light/RainbowEffect.h"
#include "light/NoiseEffect.h"
#include "light/MirrorModifier.h"
#include "light/ArtNetSendDriver.h"
#include "light/PreviewDriver.h"
#include "core/HttpServerModule.h"
#include "core/ModuleFactory.h"
#include "platform/platform.h"

#include <cstdio>

static void registerModuleTypes() {
    mm::ModuleFactory::registerType("GridLayout", []() -> mm::MoonModule* { return new mm::GridLayout(); });
    mm::ModuleFactory::registerType("RainbowEffect", []() -> mm::MoonModule* { return new mm::RainbowEffect(); });
    mm::ModuleFactory::registerType("NoiseEffect", []() -> mm::MoonModule* { return new mm::NoiseEffect(); });
    mm::ModuleFactory::registerType("MirrorModifier", []() -> mm::MoonModule* { return new mm::MirrorModifier(); });
    mm::ModuleFactory::registerType("ArtNetSendDriver", []() -> mm::MoonModule* { return new mm::ArtNetSendDriver(); });
    mm::ModuleFactory::registerType("PreviewDriver", []() -> mm::MoonModule* { return new mm::PreviewDriver(); });
}

static void printModuleTiming(mm::MoonModule* mod, int depth) {
    if (!mod) return;
    std::printf("  %s:%uus", mod->name() ? mod->name() : "?",
                static_cast<unsigned>(mod->loopTimeUs()));
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        printModuleTiming(mod->child(i), depth + 1);
    }
}

void mm_main(volatile bool& keepRunning, mm::lengthType gridW, mm::lengthType gridH, uint16_t httpPort) {
    registerModuleTypes();
    mm::Scheduler scheduler;

    // Layout
    mm::LayoutGroup layoutGroup;
    layoutGroup.setName("LayoutGroup");

    mm::GridLayout grid;
    grid.setName("Grid");
    grid.width = gridW;
    grid.height = gridH;
    layoutGroup.addChild(&grid);

    // Layer + Effect
    mm::Layer layer;
    layer.setName("Layer");
    layer.setLayoutGroup(&layoutGroup);
    layer.setChannelsPerLight(3);

    mm::NoiseEffect noise;
    noise.setName("Noise");
    layer.addChild(&noise);

    // Modifier
    mm::MirrorModifier mirror;
    mirror.setName("Mirror");
    layer.addChild(&mirror);

    // Driver Group + ArtNet
    mm::DriverGroup driverGroup;
    driverGroup.setName("DriverGroup");
    driverGroup.setLayer(&layer);

    mm::ArtNetSendDriver artnet;
    artnet.setName("ArtNet");
    driverGroup.addChild(&artnet);

    // Preview driver (WebSocket binary frames)
    mm::PreviewFrame previewFrame;
    mm::PreviewDriver preview;
    preview.setName("Preview");
    preview.width = gridW;
    preview.height = gridH;
    preview.setPreviewFrame(&previewFrame);
    driverGroup.addChild(&preview);

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

    while (keepRunning) {
        scheduler.tick();

        // Log every second
        uint32_t now = mm::platform::millis();
        if (now - lastLog >= 1000) {
            lastLog = now;
            if (scheduler.tickTimeUs() == 0) continue; // no measurement yet

            heap = mm::platform::freeHeap();
            std::printf("tick: %uus (FPS: %u)", static_cast<unsigned>(scheduler.tickTimeUs()),
                        static_cast<unsigned>(scheduler.fps()));
            if (heap > 0) {
                std::printf("  free: %u  maxBlock: %u",
                            static_cast<unsigned>(heap),
                            static_cast<unsigned>(mm::platform::maxAllocBlock()));
            }
            // Per-module timing (walk tree recursively)
            for (uint8_t i = 0; i < scheduler.moduleCount(); i++) {
                printModuleTiming(scheduler.module(i), 0);
            }
            std::printf("\n");
            std::fflush(stdout);
        }

        mm::platform::yield();
    }

    std::printf("\nShutting down.\n");
    scheduler.teardown();
}
