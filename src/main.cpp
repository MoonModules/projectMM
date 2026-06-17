#include "core/Scheduler.h"
#include "light/layers/Layers.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/SphereLayout.h"
#include "light/layouts/WheelLayout.h"
#include "light/effects/LinesEffect.h"
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
#include "light/effects/RingsEffect.h"
#include "light/effects/RipplesEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/effects/GameOfLifeEffect.h"
#include "light/effects/NetworkReceiveEffect.h"
#include "light/effects/AudioVolumeEffect.h"
#include "light/effects/AudioSpectrumEffect.h"
#include "light/effects/SineEffect.h"
#include "light/effects/DistortionWavesEffect.h"
#include "light/modifiers/MultiplyModifier.h"
#include "light/modifiers/CheckerboardModifier.h"
#include "light/modifiers/RandomMapModifier.h"
#include "light/modifiers/RotateModifier.h"
#include "light/drivers/NetworkSendDriver.h"
#include "light/drivers/PreviewDriver.h"
// LED drivers are compiled in per chip, gated on the SOC peripheral the driver
// needs — so a board's binary carries only the drivers its silicon can actually
// run (no dead flash for an LCD_CAM driver on a chip without LCD_CAM). The same
// SOC macros back the rmtTxChannels / lcdLanes / parlioLanes capability flags in
// platform_config.h. Undefined on desktop, so none compile there.
#if defined(CONFIG_SOC_RMT_SUPPORTED)
#include "light/drivers/RmtLedDriver.h"
#endif
#if defined(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED)
#include "light/drivers/LcdLedDriver.h"
#endif
#if defined(CONFIG_SOC_PARLIO_SUPPORTED)
#include "light/drivers/ParlioLedDriver.h"
#endif
#include "core/HttpServerModule.h"
#include "core/SystemModule.h"
#include "core/BoardModule.h"
#include "core/AudioModule.h"
#include "core/FirmwareUpdateModule.h"
#include "core/ImprovProvisioningModule.h"
#include "core/DevicesModule.h"
#include "core/FilesystemModule.h"
#include "core/ModuleFactory.h"
#include "platform/platform.h"

#include "core/NetworkModule.h"

#include <cstdio>

static void registerModuleTypes() {
    // Second argument is the module's spec page relative to docs/moonmodules/ —
    // the UI builds a help link from it. Filename always matches the type name.
    // Containers
    mm::ModuleFactory::registerType<mm::Layouts>("Layouts", "light/Layouts.md");
    mm::ModuleFactory::registerType<mm::Layers>("Layers", "light/Layers.md");
    mm::ModuleFactory::registerType<mm::Layer>("Layer", "light/Layer.md");
    mm::ModuleFactory::registerType<mm::Drivers>("Drivers", "light/Drivers.md");
    // Concrete modules. registerType<T> captures the type's dimensions() via
    // if-constexpr when present — EffectBase and ModifierBase both expose one,
    // so the UI's 📏/🟦/🧊 chip lights up without any per-domain wrapper.
    mm::ModuleFactory::registerType<mm::GridLayout>("GridLayout", "light/layouts/GridLayout.md");
    mm::ModuleFactory::registerType<mm::SphereLayout>("SphereLayout", "light/layouts/SphereLayout.md");
    mm::ModuleFactory::registerType<mm::WheelLayout>("WheelLayout", "light/layouts/WheelLayout.md");
    mm::ModuleFactory::registerType<mm::LinesEffect>("LinesEffect", "light/effects/LinesEffect.md");
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
    mm::ModuleFactory::registerType<mm::RingsEffect>("RingsEffect", "light/effects/RingsEffect.md");
    mm::ModuleFactory::registerType<mm::RipplesEffect>("RipplesEffect", "light/effects/RipplesEffect.md");
    mm::ModuleFactory::registerType<mm::LavaLampEffect>("LavaLampEffect", "light/effects/LavaLampEffect.md");
    mm::ModuleFactory::registerType<mm::GameOfLifeEffect>("GameOfLifeEffect", "light/effects/GameOfLifeEffect.md");
    mm::ModuleFactory::registerType<mm::NetworkReceiveEffect>("NetworkReceiveEffect", "light/effects/NetworkReceiveEffect.md");
    mm::ModuleFactory::registerType<mm::AudioVolumeEffect>("AudioVolumeEffect", "light/effects/AudioVolumeEffect.md");
    mm::ModuleFactory::registerType<mm::AudioSpectrumEffect>("AudioSpectrumEffect", "light/effects/AudioSpectrumEffect.md");
    mm::ModuleFactory::registerType<mm::SineEffect>("SineEffect", "light/effects/SineEffect.md");
    mm::ModuleFactory::registerType<mm::DistortionWavesEffect>("DistortionWavesEffect", "light/effects/DistortionWavesEffect.md");
    mm::ModuleFactory::registerType<mm::MultiplyModifier>("MultiplyModifier", "light/modifiers/MultiplyModifier.md");
    mm::ModuleFactory::registerType<mm::CheckerboardModifier>("CheckerboardModifier", "light/modifiers/CheckerboardModifier.md");
    mm::ModuleFactory::registerType<mm::RandomMapModifier>("RandomMapModifier", "light/modifiers/RandomMapModifier.md");
    mm::ModuleFactory::registerType<mm::RotateModifier>("RotateModifier", "light/modifiers/RotateModifier.md");
    mm::ModuleFactory::registerType<mm::NetworkSendDriver>("NetworkSendDriver", "light/drivers/NetworkSendDriver.md");
    mm::ModuleFactory::registerType<mm::PreviewDriver>("PreviewDriver", "light/drivers/PreviewDriver.md");
    // Register only the LED drivers this chip's silicon can run (see the gated
    // includes above) — keeps the type picker honest (no LcdLedDriver offered on a
    // chip without LCD_CAM) and the binary lean.
#if defined(CONFIG_SOC_RMT_SUPPORTED)
    mm::ModuleFactory::registerType<mm::RmtLedDriver>("RmtLedDriver", "light/drivers/RmtLedDriver.md");
#endif
#if defined(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED)
    mm::ModuleFactory::registerType<mm::LcdLedDriver>("LcdLedDriver", "light/drivers/LcdLedDriver.md");
#endif
#if defined(CONFIG_SOC_PARLIO_SUPPORTED)
    mm::ModuleFactory::registerType<mm::ParlioLedDriver>("ParlioLedDriver", "light/drivers/ParlioLedDriver.md");
#endif
    mm::ModuleFactory::registerType<mm::HttpServerModule>("HttpServerModule", "core/HttpServerModule.md");
    mm::ModuleFactory::registerType<mm::SystemModule>("SystemModule", "core/SystemModule.md");
    mm::ModuleFactory::registerType<mm::BoardModule>("BoardModule", "core/BoardModule.md");
    mm::ModuleFactory::registerType<mm::AudioModule>("AudioModule", "core/AudioModule.md");
    mm::ModuleFactory::registerType<mm::FirmwareUpdateModule>("FirmwareUpdateModule", "core/FirmwareUpdateModule.md");
    mm::ModuleFactory::registerType<mm::ImprovProvisioningModule>("ImprovProvisioningModule", "core/ImprovProvisioningModule.md");
    mm::ModuleFactory::registerType<mm::DevicesModule>("DevicesModule", "core/DevicesModule.md");
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

void mm_main(volatile bool& keepRunning, uint16_t httpPort) {
    registerModuleTypes();
    mm::Scheduler scheduler;

    // All modules created via factory (heap-allocated, PSRAM when available, classSize set)

    // Names come from ModuleFactory::create via displayNameFor — strips the
    // role suffix (Effect/Modifier/Layout/Driver, plus Module for generics) so
    // e.g. NoiseEffect → "Noise", FilesystemModule → "Filesystem". For network
    // modules the Send/Receive part is kept so NetworkSendDriver ("NetworkSend")
    // and NetworkReceiveEffect ("NetworkReceive") stay distinguishable.
    // setName() overrides are only needed for genuine renames, not for default
    // display.

    // Note: ModuleFactory::create can in principle return nullptr (factory entry
    // missing, OOM at probe construction). We deliberately do not null-check
    // each result here — these are startup-time allocations and the right
    // behaviour on failure is "device can't boot the light pipeline; crash with
    // a clear stack trace so the operator can fix the build/config." On ESP32
    // the panic handler reports a usable backtrace; on desktop the segfault
    // surfaces under gdb/lldb just as cleanly. Wrapping every line in an
    // if(!x) std::abort() pattern would add ~20 lines of boilerplate without
    // improving the observed failure mode.

    // Filesystem (first — wires the load hook into the scheduler so persisted values
    // overlay into other modules' bound variables before their setup() runs)
    auto* filesystemModule = static_cast<mm::FilesystemModule*>(mm::ModuleFactory::create("FilesystemModule"));
    filesystemModule->setScheduler(&scheduler);

    // System (deviceName needed by other modules)
    auto* systemModule = static_cast<mm::SystemModule*>(mm::ModuleFactory::create("SystemModule"));
    systemModule->setScheduler(&scheduler);

    // BoardModule — owns the physical-board identity (e.g. "olimex-esp32-gateway-rev-g").
    // Code-wired child of System; markWiredByCode preserves it on devices whose
    // saved SystemModule.json predates the addition (same mechanic that kept
    // Improv alive under Network).
    auto* boardModule = static_cast<mm::BoardModule*>(mm::ModuleFactory::create("BoardModule"));
    systemModule->addChild(boardModule);
    boardModule->markWiredByCode();

    // AudioModule is NOT auto-wired. It is a mic peripheral, useful only on a board
    // that actually has an I2S microphone, so the user adds it through the UI when
    // they have one (the same model as the effects: registered in the factory,
    // user-added, not boot-wired). Auto-wiring it on every flash forced an I2S init
    // on boards with no mic, which on the classic ESP32 hung setup() and boot-looped
    // the device. When added, its pins default to empty so it stays idle until the
    // user enters the real GPIOs. The audio effects reach it via the static
    // AudioModule::latestFrame(), which returns a silent frame when no mic exists.

    // FirmwareUpdate — surfaces OTA status as two read-only controls.
    // The actual flash is driven by POST /api/firmware/url; this module just
    // polls the shared globals so the WS push picks up progress.
    // setName("Firmware") overrides the factory-stripped default
    // ("FirmwareUpdate") — the card hosts the install-picker, so "Firmware"
    // reads as the user-facing concept (the picker is *how* you update it).
    auto* firmwareUpdateModule = static_cast<mm::FirmwareUpdateModule*>(
        mm::ModuleFactory::create("FirmwareUpdateModule"));
    firmwareUpdateModule->setName("Firmware");

    // Network (platform stubs return false on desktop — module is a no-op)
    auto* networkModule = static_cast<mm::NetworkModule*>(mm::ModuleFactory::create("NetworkModule"));
    networkModule->setScheduler(&scheduler);
    networkModule->setSystemModule(systemModule);

    // ImprovProvisioning — listens on UART0 for browser-/CLI-driven WiFi
    // credentials. Created after NetworkModule so its setter has a valid
    // pointer; the scheduler runs setup() on both in the same phase, so
    // construction order is what matters, not addModule order.
    //
    // Compile-time gated: on firmwares without WiFi (--firmware esp32-eth, and
    // desktop) the module is not created at all. This is the single
    // exception to main.cpp's "register everything, let modules guard
    // themselves" pattern. Rationale: Improv's only purpose is pushing WiFi
    // credentials; on a WiFi-less build there is no credential surface to
    // push to. A card showing "not supported" status was rejected as UI
    // noise that adds nothing actionable on those targets. hasImprov tracks
    // hasWiFi at compile time (platform_config.h); the discarded branch is
    // not code-generated.
    mm::ImprovProvisioningModule* improvModule = nullptr;
    if constexpr (mm::platform::hasImprov) {
        improvModule = static_cast<mm::ImprovProvisioningModule*>(
            mm::ModuleFactory::create("ImprovProvisioningModule"));
        improvModule->setSystemModule(systemModule);
        improvModule->setNetworkModule(networkModule);
        // SET_BOARD vendor RPC (command 0xFE, see platform_esp32_improv.cpp).
        // ImprovProvisioningModule's loop1s() picks up the validated payload
        // and forwards to boardModule->setBoard(), which arms the standard
        // FilesystemModule debounced save — same idiom as MoonDeck's HTTP push.
        improvModule->setBoardModule(boardModule);
        // Mark wired-by-code so applyNode's trim loop preserves it on devices
        // whose saved Network.json predates the Improv child (the upgrade case).
        improvModule->markWiredByCode();
    }

    // Layouts: top-level container; one or more layouts. Today one GridLayout,
    // which self-initialises to defaultGridSize (persistence overlays any saved
    // size before setup()). No boot-time dimensions threaded in here.
    auto* layouts = static_cast<mm::Layouts*>(mm::ModuleFactory::create("Layouts"));
    auto* grid = static_cast<mm::GridLayout*>(mm::ModuleFactory::create("GridLayout"));
    layouts->addChild(grid);

    // Layers: top-level container; one or more layers, each rendering
    // into its own buffer. Today one Layer with one effect + one modifier.
    auto* layersContainer = static_cast<mm::Layers*>(mm::ModuleFactory::create("Layers"));
    auto* layer = static_cast<mm::Layer*>(mm::ModuleFactory::create("Layer"));
    layer->setChannelsPerLight(3);
    layersContainer->addChild(layer);
    // setLayouts wires the shared Layouts to the container AND propagates to every child Layer.
    layersContainer->setLayouts(layouts);

    auto* noise = mm::ModuleFactory::create("NoiseEffect");
    layer->addChild(noise);

    // MultiplyModifier with its default mult=2 / mirror=true on X,Y reproduces
    // the old MirrorModifier-XY canonical pipeline (fold each axis in half).
    auto* multiply = mm::ModuleFactory::create("MultiplyModifier");
    layer->addChild(multiply);

    // Drivers: top-level container; one or more Driver children. Bound to the
    // Layers container — Drivers re-resolves the active Layer from it at every
    // buildState, so a Layer cleared+rebuilt via the API self-heals without
    // re-running this wiring. Binding the container (not a single Layer) is what
    // lets a driver read across N Layer buffers from one place — the hook
    // multi-layer blending uses.
    auto* drivers = static_cast<mm::Drivers*>(mm::ModuleFactory::create("Drivers"));
    drivers->setLayers(layersContainer);

    // Output drivers (NetworkSend + the LED drivers: RMT / LCD_CAM / Parlio) are
    // NOT boot-wired. They are added explicitly per board through the catalog
    // (POST /api/modules to the Drivers container), the same model AudioModule
    // uses, so a device only carries the drivers its board actually has instead of
    // every driver the chip is capable of. The Drivers container wires any child
    // generically in passBufferToDrivers() (setSourceBuffer/setLayer/setCorrection)
    // at setup()/onBuildState(), so a runtime-added driver is wired identically to
    // one added at boot, and a non-wiredByCode child persists across reboot via
    // FilesystemModule. A bare flash with no catalog inject therefore has no LED /
    // network output until a board is selected — the deliberate explicit-add model.

    // PreviewDriver is the one driver that stays boot-wired: it needs the HTTP
    // server's WS broadcaster (set below, once httpServer exists), a reference only
    // main.cpp has and the catalog can't supply. It reads the active Layer (resolved
    // by the Drivers container's setLayers above) for the light positions and the
    // sparse buffer it streams; it owns its own scratch buffers.
    auto* preview = static_cast<mm::PreviewDriver*>(mm::ModuleFactory::create("PreviewDriver"));
    drivers->addChild(preview);
    // Marked wired-by-code so a persistence load can't replace the wired instance
    // with a fresh factory one that lost its broadcaster — same protection
    // BoardModule / ImprovProvisioning use.
    preview->markWiredByCode();

    auto* httpServer = static_cast<mm::HttpServerModule*>(mm::ModuleFactory::create("HttpServerModule"));
    httpServer->port = httpPort;
    httpServer->setScheduler(&scheduler);
    // PreviewDriver pushes the coordinate table + per-frame RGB to the HTTP
    // server's WS broadcaster (HttpServerModule is-a BinaryBroadcaster). Light
    // owns the preview wire format end to end; core just writes the bytes.
    preview->setBroadcaster(httpServer);

    // Register top-level modules with scheduler (scheduler deletes on teardown).
    // Order matters: filesystem first (load hook runs before any module's setup),
    // then system (deviceName), firmwareUpdate (status surface, no deps), network
    // (hosts ImprovProvisioning as a child — same lifecycle, one less top-level
    // entry, conceptually right since Improv only exists to feed Network credentials),
    // light pipeline (Layouts → Layers → Drivers), then HTTP. The Scheduler walks
    // roots in this order each tick; child propagation happens inside each root.
    scheduler.addModule(filesystemModule);
    scheduler.addModule(systemModule);
    scheduler.addModule(firmwareUpdateModule);
    if (improvModule) networkModule->addChild(improvModule);
    // Devices: discovers other devices on the LAN. Child of Network (discovery
    // depends on the network being up); wired-by-code so persistence preserves it
    // on devices whose saved Network.json predates the child (see DevicesModule.md).
    auto* devicesModule = static_cast<mm::DevicesModule*>(
        mm::ModuleFactory::create("DevicesModule"));
    devicesModule->markWiredByCode();
    // Wire our own name so the self row in the device list matches the rest of the
    // device's identity (status page / router / mDNS). deviceName has static lifetime
    // (SystemModule's member); the module borrows the pointer.
    devicesModule->setSelfName(systemModule->deviceName());
    networkModule->addChild(devicesModule);
    scheduler.addModule(networkModule);
    scheduler.addModule(layouts);
    scheduler.addModule(layersContainer);
    scheduler.addModule(drivers);
    scheduler.addModule(httpServer);

    scheduler.setup();

    uint32_t lights = layouts->totalLightCount();
    uint32_t bufBytes = lights * 3;
    std::printf("projectMM running — grid %dx%d, %lu lights, buffer %lu bytes\n",
                grid->width, grid->height,
                static_cast<unsigned long>(lights),
                static_cast<unsigned long>(bufBytes));
    std::printf("sizeof: MoonModule=%zu Layer=%zu Drivers=%zu Grid=%zu HttpServer=%zu\n",
                sizeof(mm::MoonModule), sizeof(mm::Layer), sizeof(mm::Drivers),
                sizeof(mm::GridLayout), sizeof(mm::HttpServerModule));
    // NetworkSend is no longer boot-wired (added per board via the catalog), so
    // there is no boot-time instance whose IP we could log here.
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
    const uint32_t bootMillis = lastLog;   // window start for the MM_IP serial token
    bool mmIpWindowClosed = false;         // latches true once the 60 s window elapses

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
                // maxInternalAllocBlock — internal RAM only. The all-memory
                // variant reports ~8 MB on S3/S2 PSRAM boards and is useless
                // as a memory-pressure KPI. See platform.h for the split.
                std::printf("  free: %u  maxBlock: %u",
                            static_cast<unsigned>(heap),
                            static_cast<unsigned>(mm::platform::maxInternalAllocBlock()));
            }
            // Stable MM_IP=<ip> token for the web installer's post-flash serial
            // read. It rides this already-periodic line (zero extra printf, re-emits
            // every second so the installer catches it whenever it reopens the port).
            // Gated to the first 60 s of uptime: the installer reads at ~3–15 s after
            // boot, well inside that window; afterwards the device's IP comes from the
            // REST API (http://<ip>/api/…), so a permanent token would just be noise on
            // the perf line. The window latches off once for good — a plain `now <
            // 60000` would re-open every ~49.7 days when millis() wraps. All-zero
            // octets until the network connects — printed only once there's an IP.
            // Both buffers are reused stack locals, no allocation.
            if (!mmIpWindowClosed) {
                if (now - bootMillis >= 60000) {
                    mmIpWindowClosed = true;   // first 60 s elapsed; stop for the rest of uptime
                } else {
                    uint8_t ip[4];
                    networkModule->currentIp(ip);
                    if (ip[0] || ip[1] || ip[2] || ip[3]) {
                        char ipStr[16];
                        mm::formatDottedQuad(ipStr, ip);
                        std::printf("  MM_IP=%s", ipStr);
                    }
                }
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
}
