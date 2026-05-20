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
    // Containers
    mm::ModuleFactory::registerType<mm::LayoutGroup>("LayoutGroup");
    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::DriverGroup>("DriverGroup");
    // Concrete modules
    mm::ModuleFactory::registerType<mm::GridLayout>("GridLayout");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
    mm::ModuleFactory::registerType<mm::MirrorModifier>("MirrorModifier");
    mm::ModuleFactory::registerType<mm::ArtNetSendDriver>("ArtNetSendDriver");
    mm::ModuleFactory::registerType<mm::PreviewDriver>("PreviewDriver");
    mm::ModuleFactory::registerType<mm::HttpServerModule>("HttpServerModule");
}

static void printModuleMetrics(mm::MoonModule* mod, int depth) {
    if (!mod) return;
    if (mod->dynamicBytes() > 0) {
        std::printf("  %s:%uus/%uKB", mod->name() ? mod->name() : "?",
                    static_cast<unsigned>(mod->loopTimeUs()),
                    static_cast<unsigned>(mod->dynamicBytes() / 1024));
    } else {
        std::printf("  %s:%uus", mod->name() ? mod->name() : "?",
                    static_cast<unsigned>(mod->loopTimeUs()));
    }
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        printModuleMetrics(mod->child(i), depth + 1);
    }
}

void mm_main(volatile bool& keepRunning, mm::lengthType gridW, mm::lengthType gridH, uint16_t httpPort) {
    registerModuleTypes();
    mm::Scheduler scheduler;

    // All modules created via factory (heap-allocated, PSRAM when available, classSize set)
    auto* layoutGroup = static_cast<mm::LayoutGroup*>(mm::ModuleFactory::create("LayoutGroup"));
    auto* grid = static_cast<mm::GridLayout*>(mm::ModuleFactory::create("GridLayout"));
    grid->setName("Grid");
    grid->width = gridW;
    grid->height = gridH;
    layoutGroup->addChild(grid);

    auto* layer = static_cast<mm::Layer*>(mm::ModuleFactory::create("Layer"));
    layer->setLayoutGroup(layoutGroup);
    layer->setChannelsPerLight(3);

    auto* noise = mm::ModuleFactory::create("NoiseEffect");
    noise->setName("Noise");
    layer->addChild(noise);

    auto* mirror = mm::ModuleFactory::create("MirrorModifier");
    mirror->setName("Mirror");
    layer->addChild(mirror);

    auto* driverGroup = static_cast<mm::DriverGroup*>(mm::ModuleFactory::create("DriverGroup"));
    driverGroup->setLayer(layer);

    auto* artnet = mm::ModuleFactory::create("ArtNetSendDriver");
    artnet->setName("ArtNet");
    driverGroup->addChild(artnet);

    auto* previewFrame = new mm::PreviewFrame();
    auto* preview = static_cast<mm::PreviewDriver*>(mm::ModuleFactory::create("PreviewDriver"));
    preview->setName("Preview");
    preview->width = gridW;
    preview->height = gridH;
    preview->setPreviewFrame(previewFrame);
    driverGroup->addChild(preview);

    auto* httpServer = static_cast<mm::HttpServerModule*>(mm::ModuleFactory::create("HttpServerModule"));
    httpServer->setName("HttpServer");
    httpServer->port = httpPort;
    httpServer->setScheduler(&scheduler);
    httpServer->setPreviewFrame(previewFrame);

    // Register top-level modules with scheduler (scheduler deletes on teardown)
    scheduler.addModule(layoutGroup);
    scheduler.addModule(layer);
    scheduler.addModule(driverGroup);
    scheduler.addModule(httpServer);

    scheduler.setup();

    uint32_t lights = layoutGroup->totalLightCount();
    uint32_t bufBytes = lights * 3;
    std::printf("mmv3 running — grid %dx%d, %lu lights, buffer %lu bytes\n",
                grid->width, grid->height,
                static_cast<unsigned long>(lights),
                static_cast<unsigned long>(bufBytes));
    std::printf("sizeof: MoonModule=%zu Layer=%zu DriverGroup=%zu Grid=%zu HttpServer=%zu\n",
                sizeof(mm::MoonModule), sizeof(mm::Layer), sizeof(mm::DriverGroup),
                sizeof(mm::GridLayout), sizeof(mm::HttpServerModule));
    std::printf("ArtNet → %s\n", static_cast<mm::ArtNetSendDriver*>(artnet)->ip);
    std::printf("HTTP server → http://localhost:%u\n", httpServer->port);

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
                printModuleMetrics(scheduler.module(i), 0);
            }
            std::printf("\n");
            std::fflush(stdout);
        }

        mm::platform::yield();
    }

    std::printf("\nShutting down.\n");
    scheduler.teardown();
    delete previewFrame;
}
