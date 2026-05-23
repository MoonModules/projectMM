#include "core/Scheduler.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/PlasmaPaletteEffect.h"
#include "light/effects/MetaballsEffect.h"
#include "light/effects/FireEffect.h"
#include "light/effects/ParticlesEffect.h"
#include "light/effects/GlowParticlesEffect.h"
#include "light/effects/CheckerboardEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/RipplesEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/modifiers/MirrorModifier.h"
#include "light/drivers/ArtNetSendDriver.h"
#include "light/drivers/PreviewDriver.h"
#include "core/HttpServerModule.h"
#include "core/SystemModule.h"
#include "core/FilesystemModule.h"
#include "core/ModuleFactory.h"
#include "platform/platform.h"

#include "core/NetworkModule.h"

#include <cstdio>

static void registerModuleTypes() {
    // Second argument is the module's spec page relative to docs/moonmodules/ —
    // the UI builds a help link from it. Filename always matches the type name.
    // Containers
    mm::ModuleFactory::registerType<mm::LayoutGroup>("LayoutGroup", "light/LayoutGroup.md");
    mm::ModuleFactory::registerType<mm::Layer>("Layer", "light/Layer.md");
    mm::ModuleFactory::registerType<mm::DriverGroup>("DriverGroup", "light/DriverGroup.md");
    // Concrete modules. registerType<T> captures the type's dimensions() via
    // if-constexpr when present — EffectBase and ModifierBase both expose one,
    // so the UI's 📏/🟦/🧊 chip lights up without any per-domain wrapper.
    mm::ModuleFactory::registerType<mm::GridLayout>("GridLayout", "light/layouts/GridLayout.md");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect", "light/effects/RainbowEffect.md");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect", "light/effects/NoiseEffect.md");
    mm::ModuleFactory::registerType<mm::PlasmaEffect>("PlasmaEffect", "light/effects/PlasmaEffect.md");
    mm::ModuleFactory::registerType<mm::PlasmaPaletteEffect>("PlasmaPaletteEffect", "light/effects/PlasmaPaletteEffect.md");
    mm::ModuleFactory::registerType<mm::MetaballsEffect>("MetaballsEffect", "light/effects/MetaballsEffect.md");
    mm::ModuleFactory::registerType<mm::FireEffect>("FireEffect", "light/effects/FireEffect.md");
    mm::ModuleFactory::registerType<mm::ParticlesEffect>("ParticlesEffect", "light/effects/ParticlesEffect.md");
    mm::ModuleFactory::registerType<mm::GlowParticlesEffect>("GlowParticlesEffect", "light/effects/GlowParticlesEffect.md");
    mm::ModuleFactory::registerType<mm::CheckerboardEffect>("CheckerboardEffect", "light/effects/CheckerboardEffect.md");
    mm::ModuleFactory::registerType<mm::SpiralEffect>("SpiralEffect", "light/effects/SpiralEffect.md");
    mm::ModuleFactory::registerType<mm::RipplesEffect>("RipplesEffect", "light/effects/RipplesEffect.md");
    mm::ModuleFactory::registerType<mm::LavaLampEffect>("LavaLampEffect", "light/effects/LavaLampEffect.md");
    mm::ModuleFactory::registerType<mm::MirrorModifier>("MirrorModifier", "light/modifiers/MirrorModifier.md");
    mm::ModuleFactory::registerType<mm::ArtNetSendDriver>("ArtNetSendDriver", "light/drivers/ArtNetSendDriver.md");
    mm::ModuleFactory::registerType<mm::PreviewDriver>("PreviewDriver", "light/drivers/PreviewDriver.md");
    mm::ModuleFactory::registerType<mm::HttpServerModule>("HttpServerModule", "core/HttpServerModule.md");
    mm::ModuleFactory::registerType<mm::SystemModule>("SystemModule", "core/SystemModule.md");
    mm::ModuleFactory::registerType<mm::NetworkModule>("NetworkModule", "core/NetworkModule.md");
    mm::ModuleFactory::registerType<mm::FilesystemModule>("FilesystemModule", "core/FilesystemModule.md");
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

    // Names come from ModuleFactory::create via displayNameFor — strips the
    // role suffix (Effect/Modifier/Layout/Driver, plus Module for generics) so
    // e.g. NoiseEffect → "Noise", FilesystemModule → "Filesystem". For drivers,
    // the Send/Receive part is kept so siblings like ArtNetSendDriver and a
    // future ArtNetReceiveDriver stay distinguishable as "ArtNetSend" and
    // "ArtNetReceive". setName() overrides are only needed for genuine renames,
    // not for default display.

    // Filesystem (first — wires the load hook into the scheduler so persisted values
    // overlay into other modules' bound variables before their setup() runs)
    auto* filesystemModule = static_cast<mm::FilesystemModule*>(mm::ModuleFactory::create("FilesystemModule"));
    filesystemModule->setScheduler(&scheduler);

    // System (deviceName needed by other modules)
    auto* systemModule = static_cast<mm::SystemModule*>(mm::ModuleFactory::create("SystemModule"));
    systemModule->setScheduler(&scheduler);

    // Network (platform stubs return false on desktop — module is a no-op)
    auto* networkModule = static_cast<mm::NetworkModule*>(mm::ModuleFactory::create("NetworkModule"));
    networkModule->setScheduler(&scheduler);
    networkModule->setSystemModule(systemModule);

    // Layout
    auto* layoutGroup = static_cast<mm::LayoutGroup*>(mm::ModuleFactory::create("LayoutGroup"));
    auto* grid = static_cast<mm::GridLayout*>(mm::ModuleFactory::create("GridLayout"));
    grid->width = gridW;
    grid->height = gridH;
    layoutGroup->addChild(grid);

    auto* layer = static_cast<mm::Layer*>(mm::ModuleFactory::create("Layer"));
    layer->setLayoutGroup(layoutGroup);
    layer->setChannelsPerLight(3);

    auto* noise = mm::ModuleFactory::create("NoiseEffect");
    layer->addChild(noise);

    auto* mirror = mm::ModuleFactory::create("MirrorModifier");
    layer->addChild(mirror);

    auto* driverGroup = static_cast<mm::DriverGroup*>(mm::ModuleFactory::create("DriverGroup"));
    driverGroup->setLayer(layer);

    auto* artnet = mm::ModuleFactory::create("ArtNetSendDriver");
    driverGroup->addChild(artnet);  // name = "ArtNetSend" (factory default) — disambiguates from a future ArtNetReceive

    auto* previewFrame = new mm::PreviewFrame();
    auto* preview = static_cast<mm::PreviewDriver*>(mm::ModuleFactory::create("PreviewDriver"));
    // PreviewDriver reads physical dimensions from Layer at frame time (via DriverGroup's
    // setLayer wiring) so runtime grid resizes are reflected in the preview header.
    preview->setPreviewFrame(previewFrame);
    driverGroup->addChild(preview);

    auto* httpServer = static_cast<mm::HttpServerModule*>(mm::ModuleFactory::create("HttpServerModule"));
    httpServer->port = httpPort;
    httpServer->setScheduler(&scheduler);
    httpServer->setPreviewFrame(previewFrame);

    // Register top-level modules with scheduler (scheduler deletes on teardown)
    // Order matters: filesystem first (load hook runs before any module's setup),
    // then system (deviceName), network, light pipeline, then HTTP
    scheduler.addModule(filesystemModule);
    scheduler.addModule(systemModule);
    scheduler.addModule(networkModule);
    scheduler.addModule(layoutGroup);
    scheduler.addModule(layer);
    scheduler.addModule(driverGroup);
    scheduler.addModule(httpServer);

    scheduler.setup();

    uint32_t lights = layoutGroup->totalLightCount();
    uint32_t bufBytes = lights * 3;
    std::printf("projectMM running — grid %dx%d, %lu lights, buffer %lu bytes\n",
                grid->width, grid->height,
                static_cast<unsigned long>(lights),
                static_cast<unsigned long>(bufBytes));
    std::printf("sizeof: MoonModule=%zu Layer=%zu DriverGroup=%zu Grid=%zu HttpServer=%zu\n",
                sizeof(mm::MoonModule), sizeof(mm::Layer), sizeof(mm::DriverGroup),
                sizeof(mm::GridLayout), sizeof(mm::HttpServerModule));
    std::printf("ArtNet → %s\n", static_cast<mm::ArtNetSendDriver*>(artnet)->ip);
    // The server binds all interfaces (INADDR_ANY) — reachable from other
    // devices on the LAN, not only localhost.
    std::printf("HTTP server → http://localhost:%u\n", httpServer->port);
    const char* hostIp = mm::platform::hostIp();
    if (hostIp && hostIp[0]) {
        std::printf("            → http://%s:%u (from the network)\n", hostIp, httpServer->port);
    }

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
