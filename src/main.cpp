#include "core/Scheduler.h"
#include "light/layers/Layers.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/SphereLayout.h"
#include "light/layouts/WheelLayout.h"
#include "light/layouts/SingleRowLayout.h"
#include "light/layouts/SingleColumnLayout.h"
#include "light/layouts/PanelLayout.h"
#include "light/layouts/CubeLayout.h"
#include "light/layouts/TubesLayout.h"
#include "light/layouts/RingLayout.h"
#include "light/layouts/Rings241Layout.h"
#include "light/layouts/SpiralLayout.h"
#include "light/layouts/PanelsLayout.h"
#include "light/layouts/HumanSizedCubeLayout.h"
#include "light/layouts/TorontoBarGourdsLayout.h"
#include "light/layouts/CarLightsLayout.h"
#include "light/effects/LinesEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/WaveEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/MetaballsEffect.h"
#include "light/effects/FireEffect.h"
#include "light/effects/ParticlesEffect.h"
#include "light/moonlive/MoonLiveEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/RingsEffect.h"
#include "light/effects/RipplesEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/effects/NetworkReceiveEffect.h"
#include "light/effects/AudioVolumeEffect.h"
#include "light/effects/AudioSpectrumEffect.h"
#include "light/effects/SineEffect.h"
#include "light/effects/DistortionWavesEffect.h"
#include "light/effects/GameOfLifeEffect.h"
#include "light/effects/GEQ3DEffect.h"
#include "light/effects/PaintBrushEffect.h"
#include "light/effects/SolidEffect.h"
#include "light/effects/StarSkyEffect.h"
#include "light/effects/SphereMoveEffect.h"
#include "light/effects/StarFieldEffect.h"
#include "light/effects/PraxisEffect.h"
#include "light/effects/FixedRectangleEffect.h"
#include "light/effects/RandomEffect.h"
#include "light/effects/LissajousEffect.h"
#include "light/effects/RubiksCubeEffect.h"
#include "light/effects/Noise2DEffect.h"
#include "light/effects/BouncingBallsEffect.h"
#include "light/effects/TetrixEffect.h"
#include "light/effects/TextEffect.h"
#include "light/effects/FreqSawsEffect.h"
#include "light/effects/BlurzEffect.h"
#include "light/effects/FreqMatrixEffect.h"
#include "light/effects/GEQEffect.h"
#include "light/effects/NoiseMeterEffect.h"
#include "light/effects/DemoReelEffect.h"
#include "light/modifiers/MultiplyModifier.h"
#include "light/modifiers/CheckerboardModifier.h"
#include "light/modifiers/RandomMapModifier.h"
#include "light/modifiers/RotateModifier.h"
#include "light/modifiers/RegionModifier.h"
#include "light/modifiers/MirrorModifier.h"
#include "light/modifiers/TransposeModifier.h"
#include "light/modifiers/CircleModifier.h"
#include "light/modifiers/BlockModifier.h"
#include "light/modifiers/PinwheelModifier.h"
#include "light/modifiers/RippleXZModifier.h"
#include "light/drivers/Drivers.h"   // the Drivers container (registered + wired below); driver subclasses include DriverBase.h directly
#include "light/drivers/HueDriver.h"
#include "light/drivers/NetworkSendDriver.h"
#include "light/drivers/PreviewDriver.h"
// LED drivers are compiled in per chip, gated on the SOC peripheral the driver
// needs — so a board's binary carries only the drivers its silicon can actually
// run (no dead flash for an LCD_CAM driver on a chip without LCD_CAM). The same
// SOC macros back the rmtTxChannels / lcdLanes / parlioLanes capability flags in
// platform_config.h. Undefined on desktop, so none compile there.
//
// Why the preprocessor here and not `if constexpr` (the usual platform-branch
// form): the goal is to *exclude the unused driver's code from the binary*, and
// `if constexpr` still compiles every branch — it can't drop the include. The
// include and the registration must share one condition (a missing include means
// the type isn't declared), so both are `#if`. These are SOC-*capability* macros
// (CONFIG_SOC_*), NOT the platform/OS `#ifdef`s the boundary rule forbids
// (ESP_PLATFORM / CONFIG_IDF_TARGET_* / __APPLE__ / …) — check_platform_boundary.py
// passes them, by design. The driver bodies themselves keep all hardware behind
// the platform seam; this gate only decides which driver headers are present.
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
#include "core/Services.h"
#include "core/AudioService.h"
#include "core/I2cScanModule.h"
#include "core/TasksModule.h"
#include "core/PinsModule.h"
#include "core/IrService.h"
#include "core/FileManagerModule.h"
#include "core/FirmwareUpdateModule.h"
#include "core/BleProvisioningModule.h"
#include "core/ImprovProvisioningModule.h"
#include "core/MqttModule.h"
#include "core/DevicesModule.h"
#include "core/FilesystemModule.h"
#include "core/ModuleFactory.h"
#include "platform/platform.h"

#include "core/NetworkModule.h"

#include <cstdio>

static void registerModuleTypes() {
    // Second argument is the module's spec page relative to docs/moonmodules/ —
    // the UI builds a help link from it. Effects/modifiers/leaf-layouts share one
    // compact-row page per type (effects.md, …); containers, drivers, and
    // core modules keep a per-module page named for the type.
    // Containers
    mm::ModuleFactory::registerType<mm::Layouts>("Layouts", "light/supporting.md#layouts");
    mm::ModuleFactory::registerType<mm::Layers>("Layers", "light/supporting.md#layers");
    mm::ModuleFactory::registerType<mm::Layer>("Layer", "light/supporting.md#layer");
    mm::ModuleFactory::registerType<mm::Drivers>("Drivers", "light/supporting.md#drivers");
    // Concrete modules. registerType<T> captures the type's dimensions() via
    // if-constexpr when present — EffectBase and ModifierBase both expose one,
    // so the UI's 📏/🟦/🧊 chip lights up without any per-domain wrapper.
    // Layouts — alphabetical by display name.
    mm::ModuleFactory::registerType<mm::CarLightsLayout>("CarLightsLayout", "light/layouts.md#carlights");
    mm::ModuleFactory::registerType<mm::CubeLayout>("CubeLayout", "light/layouts.md#cube");
    mm::ModuleFactory::registerType<mm::HumanSizedCubeLayout>("HumanSizedCubeLayout", "light/layouts.md#humansizedcube");
    mm::ModuleFactory::registerType<mm::PanelsLayout>("PanelsLayout", "light/layouts.md#panels");
    mm::ModuleFactory::registerType<mm::TorontoBarGourdsLayout>("TorontoBarGourdsLayout", "light/layouts.md#torontobargourds");
    mm::ModuleFactory::registerType<mm::GridLayout>("GridLayout", "light/layouts.md#grid");
    mm::ModuleFactory::registerType<mm::PanelLayout>("PanelLayout", "light/layouts.md#panel");
    mm::ModuleFactory::registerType<mm::RingLayout>("RingLayout", "light/layouts.md#ring");
    mm::ModuleFactory::registerType<mm::Rings241Layout>("Rings241Layout", "light/layouts.md#rings241");
    mm::ModuleFactory::registerType<mm::SingleColumnLayout>("SingleColumnLayout", "light/layouts.md#singlecolumn");
    mm::ModuleFactory::registerType<mm::SingleRowLayout>("SingleRowLayout", "light/layouts.md#singlerow");
    mm::ModuleFactory::registerType<mm::SphereLayout>("SphereLayout", "light/layouts.md#sphere");
    mm::ModuleFactory::registerType<mm::SpiralLayout>("SpiralLayout", "light/layouts.md#spiral");
    mm::ModuleFactory::registerType<mm::TubesLayout>("TubesLayout", "light/layouts.md#tubes");
    mm::ModuleFactory::registerType<mm::WheelLayout>("WheelLayout", "light/layouts.md#wheel");
    // Effects — registered alphabetically by display name (the picker + docs also sort
    // alphabetically; keeping this list sorted makes the three orders agree at a glance).
    mm::ModuleFactory::registerType<mm::AudioSpectrumEffect>("AudioSpectrumEffect", "light/effects.md#audiospectrum");
    mm::ModuleFactory::registerType<mm::AudioVolumeEffect>("AudioVolumeEffect", "light/effects.md#audiovolume");
    mm::ModuleFactory::registerType<mm::BlurzEffect>("BlurzEffect", "light/effects.md#blurz");
    mm::ModuleFactory::registerType<mm::BouncingBallsEffect>("BouncingBallsEffect", "light/effects.md#bouncingballs");
    mm::ModuleFactory::registerType<mm::DemoReelEffect>("DemoReelEffect", "light/effects.md#demoreel");
    mm::ModuleFactory::registerType<mm::DistortionWavesEffect>("DistortionWavesEffect", "light/effects.md#distortionwaves");
    mm::ModuleFactory::registerType<mm::FireEffect>("FireEffect", "light/effects.md#fire");
    mm::ModuleFactory::registerType<mm::FixedRectangleEffect>("FixedRectangleEffect", "light/effects.md#fixedrectangle");
    mm::ModuleFactory::registerType<mm::FreqMatrixEffect>("FreqMatrixEffect", "light/effects.md#freqmatrix");
    mm::ModuleFactory::registerType<mm::FreqSawsEffect>("FreqSawsEffect", "light/effects.md#freqsaws");
    mm::ModuleFactory::registerType<mm::GameOfLifeEffect>("GameOfLifeEffect", "light/effects.md#gameoflife");
    mm::ModuleFactory::registerType<mm::GEQEffect>("GEQEffect", "light/effects.md#geq");
    mm::ModuleFactory::registerType<mm::GEQ3DEffect>("GEQ3DEffect", "light/effects.md#geq3d");
    mm::ModuleFactory::registerType<mm::LavaLampEffect>("LavaLampEffect", "light/effects.md#lavalamp");
    mm::ModuleFactory::registerType<mm::LinesEffect>("LinesEffect", "light/effects.md#lines");
    mm::ModuleFactory::registerType<mm::LissajousEffect>("LissajousEffect", "light/effects.md#lissajous");
    mm::ModuleFactory::registerType<mm::MetaballsEffect>("MetaballsEffect", "light/effects.md#metaballs");
    mm::ModuleFactory::registerType<mm::MoonLiveEffect>("MoonLiveEffect", "light/MoonLiveEffect.md");
    mm::ModuleFactory::registerType<mm::NetworkReceiveEffect>("NetworkReceiveEffect", "light/effects.md#networkreceive");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect", "light/effects.md#noise");
    mm::ModuleFactory::registerType<mm::Noise2DEffect>("Noise2DEffect", "light/effects.md#noise2d");
    mm::ModuleFactory::registerType<mm::NoiseMeterEffect>("NoiseMeterEffect", "light/effects.md#noisemeter");
    mm::ModuleFactory::registerType<mm::PaintBrushEffect>("PaintBrushEffect", "light/effects.md#paintbrush");
    mm::ModuleFactory::registerType<mm::ParticlesEffect>("ParticlesEffect", "light/effects.md#particles");
    mm::ModuleFactory::registerType<mm::PlasmaEffect>("PlasmaEffect", "light/effects.md#plasma");
    mm::ModuleFactory::registerType<mm::PraxisEffect>("PraxisEffect", "light/effects.md#praxis");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect", "light/effects.md#rainbow");
    mm::ModuleFactory::registerType<mm::RandomEffect>("RandomEffect", "light/effects.md#random");
    mm::ModuleFactory::registerType<mm::RingsEffect>("RingsEffect", "light/effects.md#rings");
    mm::ModuleFactory::registerType<mm::RipplesEffect>("RipplesEffect", "light/effects.md#ripples");
    mm::ModuleFactory::registerType<mm::RubiksCubeEffect>("RubiksCubeEffect", "light/effects.md#rubikscube");
    mm::ModuleFactory::registerType<mm::SineEffect>("SineEffect", "light/effects.md#sine");
    mm::ModuleFactory::registerType<mm::SolidEffect>("SolidEffect", "light/effects.md#solid");
    mm::ModuleFactory::registerType<mm::SphereMoveEffect>("SphereMoveEffect", "light/effects.md#spheremove");
    mm::ModuleFactory::registerType<mm::SpiralEffect>("SpiralEffect", "light/effects.md#spiral");
    mm::ModuleFactory::registerType<mm::StarFieldEffect>("StarFieldEffect", "light/effects.md#starfield");
    mm::ModuleFactory::registerType<mm::StarSkyEffect>("StarSkyEffect", "light/effects.md#starsky");
    mm::ModuleFactory::registerType<mm::TetrixEffect>("TetrixEffect", "light/effects.md#tetrix");
    mm::ModuleFactory::registerType<mm::TextEffect>("TextEffect", "light/effects.md#text");
    mm::ModuleFactory::registerType<mm::WaveEffect>("WaveEffect", "light/effects.md#wave");
    // Modifiers — alphabetical by display name.
    mm::ModuleFactory::registerType<mm::BlockModifier>("BlockModifier", "light/modifiers.md#block");
    mm::ModuleFactory::registerType<mm::CheckerboardModifier>("CheckerboardModifier", "light/modifiers.md#checkerboard");
    mm::ModuleFactory::registerType<mm::CircleModifier>("CircleModifier", "light/modifiers.md#circle");
    mm::ModuleFactory::registerType<mm::MirrorModifier>("MirrorModifier", "light/modifiers.md#mirror");
    mm::ModuleFactory::registerType<mm::MultiplyModifier>("MultiplyModifier", "light/modifiers.md#multiply");
    mm::ModuleFactory::registerType<mm::PinwheelModifier>("PinwheelModifier", "light/modifiers.md#pinwheel");
    mm::ModuleFactory::registerType<mm::RandomMapModifier>("RandomMapModifier", "light/modifiers.md#randommap");
    mm::ModuleFactory::registerType<mm::RegionModifier>("RegionModifier", "light/modifiers.md#region");
    mm::ModuleFactory::registerType<mm::RippleXZModifier>("RippleXZModifier", "light/modifiers.md#ripplexz");
    mm::ModuleFactory::registerType<mm::RotateModifier>("RotateModifier", "light/modifiers.md#rotate");
    mm::ModuleFactory::registerType<mm::TransposeModifier>("TransposeModifier", "light/modifiers.md#transpose");
    mm::ModuleFactory::registerType<mm::HueDriver>("HueDriver", "light/drivers.md#hue");
    mm::ModuleFactory::registerType<mm::NetworkSendDriver>("NetworkSendDriver", "light/drivers.md#networksend");
    mm::ModuleFactory::registerType<mm::PreviewDriver>("PreviewDriver", "light/drivers.md#preview");
    // Register only the LED drivers this chip's silicon can run (see the gated
    // includes above) — keeps the type picker honest (no LcdLedDriver offered on a
    // chip without LCD_CAM) and the binary lean.
#if defined(CONFIG_SOC_RMT_SUPPORTED)
    mm::ModuleFactory::registerType<mm::RmtLedDriver>("RmtLedDriver", "light/drivers.md#rmtled");
#endif
#if defined(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED)
    mm::ModuleFactory::registerType<mm::LcdLedDriver>("LcdLedDriver", "light/drivers.md#lcdled");
#endif
#if defined(CONFIG_SOC_PARLIO_SUPPORTED)
    mm::ModuleFactory::registerType<mm::ParlioLedDriver>("ParlioLedDriver", "light/drivers.md#parlioled");
#endif
    mm::ModuleFactory::registerType<mm::HttpServerModule>("HttpServerModule", "core/system.md");
    mm::ModuleFactory::registerType<mm::SystemModule>("SystemModule", "core/system.md#system");
    mm::ModuleFactory::registerType<mm::Services>("Services", "core/services.md#services");
    mm::ModuleFactory::registerType<mm::AudioService>("AudioService", "core/services.md#audio");
    mm::ModuleFactory::registerType<mm::I2cScanModule>("I2cScanModule", "core/system.md#i2c-scan");
    mm::ModuleFactory::registerType<mm::TasksModule>("TasksModule", "core/system.md#tasks");
    mm::ModuleFactory::registerType<mm::PinsModule>("PinsModule", "core/system.md#pins");
    mm::ModuleFactory::registerType<mm::IrService>("IrService", "core/services.md#ir");
    mm::ModuleFactory::registerType<mm::FileManagerModule>("FileManagerModule", "core/system.md#file-manager");
    mm::ModuleFactory::registerType<mm::FirmwareUpdateModule>("FirmwareUpdateModule", "core/system.md#firmware-update");
    mm::ModuleFactory::registerType<mm::BleProvisioningModule>("BleProvisioningModule", "core/system.md#ble-provisioning");
    mm::ModuleFactory::registerType<mm::ImprovProvisioningModule>("ImprovProvisioningModule", "core/system.md#improv-provisioning");
    mm::ModuleFactory::registerType<mm::MqttModule>("MqttModule", "core/system.md#mqtt");
    mm::ModuleFactory::registerType<mm::DevicesModule>("DevicesModule", "core/system.md#devices");
    mm::ModuleFactory::registerType<mm::NetworkModule>("NetworkModule", "core/system.md#network");
    mm::ModuleFactory::registerType<mm::FilesystemModule>("FilesystemModule", "core/supporting.md#filesystem");
}

static void printModuleMetrics(mm::MoonModule* mod, int depth) {
    if (!mod) return;
    if (mod->dynamicBytes() > 0) {
        std::printf("  %s:%uus/%uKB", mod->name() ? mod->name() : "?",
                    static_cast<unsigned>(mod->tickTimeUs()),
                    static_cast<unsigned>(mod->dynamicBytes() / 1024));
    } else {
        std::printf("  %s:%uus", mod->name() ? mod->name() : "?",
                    static_cast<unsigned>(mod->tickTimeUs()));
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

    // File Manager — browse/manage the filesystem (a device-wide tool, boot-wired like the other
    // system modules, not per-board). Distinct from FilesystemModule (the persistence engine).
    // setName gives the card a clean "File Manager" label (the type stays FileManagerModule).
    auto* fileManagerModule = static_cast<mm::FileManagerModule*>(mm::ModuleFactory::create("FileManagerModule"));
    fileManagerModule->setName("File Manager");

    // System (deviceName needed by other modules)
    auto* systemModule = static_cast<mm::SystemModule*>(mm::ModuleFactory::create("SystemModule"));
    systemModule->setScheduler(&scheduler);

    // Fixed System modules — the device's inspection / bring-up toolkit (what runs,
    // what's on the I²C bus). Wired by code as System children (like Improv under
    // Network), not user-added: always present, no add/delete. markWiredByCode()
    // exempts them from the persistence trim, and their base Generic role means no
    // container accepts them as a user-editable child (so the card shows no delete).
    auto* tasksModule = static_cast<mm::TasksModule*>(mm::ModuleFactory::create("TasksModule"));
    tasksModule->markWiredByCode();
    systemModule->addChild(tasksModule);
    auto* i2cScanModule = static_cast<mm::I2cScanModule*>(mm::ModuleFactory::create("I2cScanModule"));
    i2cScanModule->markWiredByCode();
    systemModule->addChild(i2cScanModule);
    auto* pinsModule = static_cast<mm::PinsModule*>(mm::ModuleFactory::create("PinsModule"));
    pinsModule->markWiredByCode();
    systemModule->addChild(pinsModule);

    // Services — top-level container for user-added capability modules (Audio, IR).
    // The core-domain twin of the light domain's Layers/Drivers: a grouping node
    // whose children the user adds/removes at runtime. Added as a root below.
    auto* servicesModule = static_cast<mm::Services*>(mm::ModuleFactory::create("Services"));

    // The deviceModel identity (e.g. "Olimex ESP32-Gateway Rev G") is now SystemModule's
    // `deviceModel` control — no separate module. SystemModule owns the device identity
    // (deviceName + deviceModel) directly; tooling injects deviceModel like any catalog
    // default — via /api/control (MoonDeck) or an APPLY_OP `set System.deviceModel` over
    // serial (the installer) — both routed through the apply-core + the control's validator.

    // AudioService is NOT auto-wired. It is a mic peripheral, useful only on a board
    // that actually has an I2S microphone, so the user adds it through the UI when
    // they have one (the same model as the effects: registered in the factory,
    // user-added, not boot-wired). Auto-wiring it on every flash forced an I2S init
    // on boards with no mic, which on the classic ESP32 hung setup() and boot-looped
    // the device. When added, its pins default to empty so it stays idle until the
    // user enters the real GPIOs. The audio effects reach it via the static
    // AudioService::latestFrame(), which returns a silent frame when no mic exists.

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
        // systemModule is wired for GET_DEVICE_INFO (the device name) and networkModule
        // for WIFI_SETTINGS credentials; deviceModel arrives as an APPLY_OP
        // `set System.deviceModel`, like any catalog default.
        // Mark wired-by-code so applyNode's trim loop preserves it on devices
        // whose saved Network.json predates the Improv child (the upgrade case).
        improvModule->markWiredByCode();
    }

    // BLE provisioning uses Espressif's standard phone-app protocol. It is a
    // Network child for the same reason as Improv: the transport publishes
    // credentials; NetworkModule owns persistence and connection state.
    mm::BleProvisioningModule* bleProvisioningModule = nullptr;
    if constexpr (mm::platform::hasBleProvisioning) {
        bleProvisioningModule = static_cast<mm::BleProvisioningModule*>(
            mm::ModuleFactory::create("BleProvisioningModule"));
        bleProvisioningModule->setName("BLE Provision");
        bleProvisioningModule->setSystemModule(systemModule);
        bleProvisioningModule->setNetworkModule(networkModule);
        bleProvisioningModule->markWiredByCode();
    }

    // MQTT service: a code-wired child of Network (like Improv), bridging the light controls to a
    // broker for Homebridge/Home-Assistant. Built on every networked target (it uses TCP, so it
    // works over WiFi or Ethernet); disabled until the user sets a broker. systemModule is injected
    // for the default topic prefix (the device name).
    mm::MqttModule* mqttModule = nullptr;
    if constexpr (mm::platform::hasNetwork) {
        mqttModule = static_cast<mm::MqttModule*>(mm::ModuleFactory::create("MqttModule"));
        mqttModule->setSystemModule(systemModule);
        mqttModule->markWiredByCode();
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

    // One default effect so a bare device (no catalog inject) still shows lights out
    // of the box — but NO default modifier: the boot Layer is just an effect on a
    // 16x16 grid. A device-model catalog entry can REPLACE this (replaceChildren) with
    // its own effects/modifiers — e.g. the testbench swaps in AudioSpectrum + RandomMap.
    auto* noise = mm::ModuleFactory::create("NoiseEffect");
    layer->addChild(noise);

    // Drivers: top-level container; one or more Driver children. Bound to the
    // Layers container — Drivers re-resolves the active Layer from it at every
    // prepareTree, so a Layer cleared+rebuilt via the API self-heals without
    // re-running this wiring. Binding the container (not a single Layer) is what
    // lets a driver read across N Layer buffers from one place — the hook
    // multi-layer blending uses.
    auto* drivers = static_cast<mm::Drivers*>(mm::ModuleFactory::create("Drivers"));
    drivers->setLayers(layersContainer);

    // Output drivers (NetworkSend + the LED drivers: RMT / LCD_CAM / Parlio) are
    // NOT boot-wired. They are added explicitly per board through the catalog
    // (POST /api/modules to the Drivers container), the same model AudioService
    // uses, so a device only carries the drivers its board actually has instead of
    // every driver the chip is capable of. The Drivers container wires any child
    // generically in passBufferToDrivers() (setSourceBuffer/setLayer/setCorrection)
    // at setup()/prepare(), so a runtime-added driver is wired identically to
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
    // ImprovProvisioning uses.
    preview->markWiredByCode();

    auto* httpServer = static_cast<mm::HttpServerModule*>(mm::ModuleFactory::create("HttpServerModule"));
    httpServer->port = httpPort;
    httpServer->setScheduler(&scheduler);
    // PreviewDriver pushes the coordinate table + per-frame RGB to the HTTP
    // server's WS broadcaster (HttpServerModule is-a BinaryBroadcaster). Light
    // owns the preview wire format end to end; core just writes the bytes.
    preview->setBroadcaster(httpServer);

    // APPLY_OP vendor RPC (0xFC): the installer pushes the device-model's catalog
    // ops over serial during provisioning, and ImprovProvisioningModule routes each
    // to the HttpServerModule apply-core (the same code /api/modules + /api/control
    // use) — "Improv = REST over serial". Wired here once httpServer exists.
    if (improvModule) improvModule->setHttpServerModule(httpServer);

    // Register top-level modules with scheduler (scheduler deletes on release).
    // Order matters: filesystem first (load hook runs before any module's setup),
    // then system (deviceName), fileManager (filesystem browser, reads the persistence
    // engine's "last saved"), firmwareUpdate (status surface, no deps), network (hosts
    // ImprovProvisioning and Mqtt as children — same lifecycle, one less top-level entry
    // each; Improv only exists to feed Network credentials, and Mqtt only bridges once the
    // network is up), services (the user-added-capability container: Audio, IR — placed after
    // network because a service may use it, e.g. WLED audio sync, and before the light pipeline
    // so a capability like audio is available to the effects that consume it), light pipeline
    // (Layouts → Layers → Drivers), then HTTP. The Scheduler walks roots in this order each
    // tick; child propagation happens inside each root.
    scheduler.addModule(filesystemModule);
    scheduler.addModule(systemModule);
    scheduler.addModule(fileManagerModule);
    scheduler.addModule(firmwareUpdateModule);
    if (improvModule) networkModule->addChild(improvModule);
    if (bleProvisioningModule) networkModule->addChild(bleProvisioningModule);
    if (mqttModule) networkModule->addChild(mqttModule);
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
    scheduler.addModule(servicesModule);
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
                        // Alongside MM_IP: the device's mDNS .local name, so the web
                        // installer can offer a clickable http://<deviceName>.local link
                        // that survives a later DHCP IP change. deviceName is owned by
                        // SystemModule and is the device's single network identity (the
                        // SAME string NetworkModule registers for mDNS, the AP SSID, and
                        // the DHCP hostname; SystemModule keeps it a valid hostname), so
                        // this is exactly what resolves on the LAN. Serial-borne because
                        // the installer's REST fallback is blocked by mixed-content on the
                        // HTTPS Pages site.
                        std::printf("  MM_DEVICE=%s.local", systemModule->deviceName());
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
    scheduler.release();
}
