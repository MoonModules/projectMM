/// The entry point: registers every module type with the factory, builds the boot module tree, and runs the render loop.
///
/// @moreinfo
///
/// ## Why LED drivers are gated by the preprocessor
///
/// The preprocessor rather than `if constexpr`, because the goal is excluding the code and a constexpr branch still compiles every arm.
/// These are capability macros, not the platform ones the boundary rule forbids.
/// Each parallel-WS2812 backend header self-registers its factory into ParallelLedDriver's peripheral registry, gated by the chip's `CONFIG_SOC_*`.
/// So including the ones this silicon supports is what populates the `peripheral` control's options.
/// Registering only what the silicon can run keeps the type picker honest, offering no I80Peripheral on a chip without an i80 bus, and keeps the binary lean.
/// NDI, HLS and RTSP are gated by CAPABILITY instead: their headers compile everywhere, since the platform calls are declared on every target.
/// An `if constexpr` discarded branch must still PARSE, so those includes cannot be gated.
///
/// ## Why panel cards are per firmware and HUB75 is per chip
///
/// Panel receiver cards need a gigabit link, and no capability macro separates the boards that have one.
/// So the firmware catalogue names the variants that get it, and everything else saves the flash.
/// HUB75 is a GPIO panel rather than a receiver card, needing LCD_CAM or Parlio silicon and nothing else, so it gates on the chip like every other LED driver.
/// Tying it to `MM_PANEL_CARDS` hid it from every S3 that is not a panel-card firmware, which is most of them.
///
/// ## Why the quiesce-render hook is a function pointer
///
/// Before core mutates the tree, adding, removing or replacing a child, core 1 stops so it cannot dereference a node being freed.
/// Core cannot name Drivers, a light module, so it calls through this function-pointer seam: see MoonModule quiesceForMutation.
/// Wired once in registerModuleTypes, where main.cpp legitimately depends on both sides.
///
/// ## What registerType captures
///
/// The second argument is the module's spec page, which the UI turns into a help link.
/// Effects, modifiers and leaf layouts share one page per type; the rest keep their own.
/// `registerType<T>` also captures the type's `dimensions()` via if-constexpr when present.
/// EffectBase and ModifierBase both expose one, so the UI's 📏/🟦/🧊 chip lights up without any per-domain wrapper.
/// Layouts, effects and modifiers are registered alphabetically by display name, matching the picker and the docs so the three orders agree at a glance.
///
/// ## How the boot tree is created
///
/// All modules are created via the factory: heap-allocated, PSRAM when available, `classSize` set.
/// Names come from the factory, which strips the role suffix, keeping the direction on a network module so send and receive stay distinguishable.
/// An explicit name is needed only for a genuine rename.
/// Creation can return null in principle, and these results are deliberately not checked.
/// At startup the right behavior is a crash with a usable backtrace, which both targets already give, rather than boilerplate that reports the same failure less clearly.
///
/// ## Why markWiredByCode matters
///
/// A boot-wired module is wiring rather than a user choice, so the persisted tree must not decide whether it exists.
/// Without the mark, a config written before a child was added drops that child on load, which is exactly what happened when Talk was introduced beside Stats.
/// The file listed one child, so the tree came back with one.
/// It also stops a persistence load replacing a wired instance with a fresh factory one that lost an injected pointer, which is what protects PreviewDriver's broadcaster.
/// Devices is the same case: the mark preserves it on a device whose saved Network.json predates the child.
///
/// ## Why the audio service is boot-wired
///
/// Auto-wiring once forced an I2S init on boards with no microphone, which hung setup and boot-looped a classic ESP32.
/// Its pins now default to empty so it idles until real ones are entered, and the effects read a silent frame when no microphone exists.
/// `mode` is synthesized by default, so a board with a microphone names it in its catalog entry the way it already names its pins.
///
/// ## Why the device-wide tools are boot-wired
///
/// ControlModule holds the presets, which are a device capability rather than something a user adds, so it exists whatever the persisted tree says.
/// File Manager is the same kind of thing: a device-wide tool rather than a per-board one, so no catalog entry adds it.
/// Its setName only changes the card's label, and the type stays FileManagerModule, which is what a persisted tree and a type lookup both key on.
///
/// ## Why MoonCloud is not a Firmware child
///
/// Parenting it under Firmware was tried: that module does not chain to its children, so anything parented there showed an empty card.
/// Each MoonCloud child carries its own consent, because wanting a joint lightshow is not agreeing to usage reporting.
/// Talk is a SECOND child with its own consent, because publishing a message and sharing a chip model are different decisions.
///
/// ## Why Improv is compile-time gated
///
/// Improv is the one exception to registering everything and letting modules guard themselves.
/// Its only purpose is pushing credentials, so on a build without WiFi there is no surface to push to.
/// It is created after the network module so its setter has a valid pointer, and one module answers the device-info request while the other takes the credentials.
/// The APPLY_OP vendor RPC (0xFC) carries the device-model's catalog ops over serial during provisioning.
/// ImprovProvisioningModule routes each to the HttpServerModule apply-core, the same code `/api/modules` and `/api/control` use: "Improv = REST over serial".
///
/// ## Why MQTT is built on every networked target
///
/// MQTT bridges the light controls to a broker for Homebridge and Home Assistant.
/// It is built on every networked target because it uses TCP, so it works over WiFi or Ethernet alike, and it stays disabled until the user sets a broker.
/// systemModule is injected for the default topic prefix, which is the device name.
/// controlModule is injected so the look-only presets become the Home Assistant effect list.
///
/// ## Why the boot layer is one Pulse effect
///
/// One default effect so a bare device with no catalog inject still shows lights out of the box, but NO default modifier. The boot Layer is just an effect on a 16x16 grid.
/// A device-model catalog entry can REPLACE it through `replaceChildren` with its own effects and modifiers, the way the testbench swaps in AudioSpectrum plus RandomMap.
/// Pulse is the one because a first boot has to answer three questions at once: the lights work, the device runs, and it hears the room.
/// A sparse shell answers all three, where a dense field answers only the first since every light is already lit.
///
/// ## Why output drivers are not boot-wired
///
/// Output drivers are added per board through the catalog, so a device carries only what its board has.
/// The container wires any child generically, so one added at runtime is wired exactly like one added at boot and persists across a reboot.
/// A bare flash therefore has no output until a board is selected, which is the deliberate model.
/// The preview is the one exception: it needs the broadcaster only this file holds, which the catalog cannot supply.
/// PreviewDriver pushes the coordinate table plus per-frame RGB to the HTTP server's WS broadcaster, HttpServerModule being a BinaryBroadcaster.
/// Light owns the preview wire format end to end; core just writes the bytes.
///
/// ## Why registration order matters
///
/// The scheduler walks the roots in registration order each tick.
/// The filesystem comes first so its load hook runs before any setup, overlaying persisted values into other modules' bound variables.
/// Then the identity and status surfaces, then network with its credential and bridge children, then the services a later stage may consume, then the light pipeline, and the server last.
/// The server is added only where an IP stack exists. Setup binds a socket, and with no interface compiled in nothing initializes the stack, so its thread never exists and the board goes down before the light pipeline runs.
/// That gate is what makes a network-less build supported.
///
/// ## What the boot banner prints
///
/// The server binds every interface, so it is reachable across the LAN, and a loopback name only means anything where the browser runs on the host.
/// A device prints its interface address instead, and the network module logs the real one as each interface comes up.
/// With no address yet the line states just that, which covers two different reasons.
/// On a device it is normal this early, an interface not being up, and NetworkModule logs the address when it comes up.
/// On a desktop it means `hostIp()` found no route at all, and stating what is true promises no follow-up message an offline desktop never prints.
///
/// ## Why the render loop subscribes to the task watchdog
///
/// A genuine wedge in the render loop now panics and reboots, the self-heal, with a backtrace, instead of hanging silently.
/// The sdkconfig runs the TWDT with idle-task checking OFF, a saturated core being healthy rather than a bug, so this explicit subscription is what the watchdog actually watches.
/// It is reset each tick, so a heavy-but-live frame keeps feeding it.
///
/// ## Why the periodic line is a plain write
///
/// A plain write rather than a log call, so the platform level cannot suppress it, and this gates on the same level by hand.
/// Silenced above a warning, so a resting device makes no periodic serial write and a transmit-blinking LED stops flickering.
/// The first minute always prints, because the installer reads the address off this line.
/// `maxInternalAllocBlock` reports internal RAM only: the all-memory variant reports about 8 MB on S3/S2 PSRAM boards and is useless as a memory-pressure KPI, and platform.h holds the split.
/// renderWait is the worst wait at a frame boundary in the last second rather than a single frame's, which would land wherever this once-a-second line falls and read as nothing.
/// It is what says whether a second buffer would recover idle time or gain nothing.
///
/// ## Why the address token rides the periodic line
///
/// MM_IP is a stable address token for the installer's post-flash serial read, riding an already-periodic line so it costs no extra write and repeats until the port is reopened.
/// It is gated to the first minute, after which the address comes from the API, and the window latches off for good so a counter wrap cannot reopen it.
/// MM_DEVICE carries the discovery name alongside the address, so the installer's link survives a lease change.
/// It is the one network identity, and it rides serial because the installer's fallback is blocked by mixed content.
///
/// ## Why the loop pacing uses goto and a sleep
///
/// The loop's pacing lives at its TAIL, so a `continue` in the logging block would skip the yield and spin the core for that pass.
/// Jumping to the pacing point keeps "skip the logging" from meaning "skip the sleep".
/// Yielding only offers the CPU to another runnable thread, so with nothing else to run it returns at once and this burns a whole core, as a bench reported.
/// A sub-millisecond sleep parks the thread at no cost to the frame rate, and the yield stays for the split's frame boundary, which must not sleep.
///
#include "core/module/Scheduler.h"
#include "light/layers/Effects.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/GridBlacksLayout.h"
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
#include "light/effects/FluidEffect.h"
#include "light/effects/NebulaEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/FixedPointEffect.h"
#include "light/effects/MovingHeadEffect.h"
#include "light/effects/PacmanEffect.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/PulseEffect.h"
#include "light/effects/MetaballsEffect.h"
#include "light/effects/FireEffect.h"
#include "light/effects/ParticlesEffect.h"
#include "light/moonlive/MoonLiveEffect.h"
#include "light/moonlive/MoonLiveModifier.h"
#include "light/moonlive/MoonLiveLayout.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/RingsEffect.h"
#include "light/effects/RipplesEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/effects/NetworkReceiveEffect.h"
#include "light/effects/RadialSpectrumEffect.h"
#include "light/effects/VuMetersEffect.h"
#include "light/effects/BeatRipplesEffect.h"
#include "light/effects/AudioSpectrumEffect.h"
#include "light/effects/SineEffect.h"
#include "light/effects/DistortionWavesEffect.h"
#include "light/effects/GameOfLifeEffect.h"
#include "light/effects/GEQ3DEffect.h"
#include "light/effects/PaintBrushEffect.h"
#include "light/effects/SolidEffect.h"
#include "light/effects/StarSkyEffect.h"
#include "light/effects/SdfShapesEffect.h"
#include "light/effects/AuroraEffect.h"
#include "light/effects/PolarNoiseEffect.h"
#include "light/effects/WaterRippleEffect.h"
#include "light/effects/TrailsEffect.h"
#include "light/effects/ColorTrailsEffect.h"
#include "light/effects/TunnelEffect.h"
#include "light/effects/EchoEffect.h"
#include "light/effects/DissolveEffect.h"
#include "light/effects/SpectrumEffect.h"
#include "light/effects/FireworksEffect.h"
#include "light/effects/BallpitEffect.h"
#include "light/effects/FishTankEffect.h"
#include "light/effects/FlyingToastersEffect.h"
#include "light/effects/PongEffect.h"
#include "light/effects/SpaceInvadersEffect.h"
#include "light/effects/SpriteFountainEffect.h"
#include "light/effects/TruchetEffect.h"
#include "light/effects/VectorBallsEffect.h"
#include "light/effects/RaymarchEffect.h"
#include "light/effects/SphereMoveEffect.h"
#include "light/effects/StarFieldEffect.h"
#include "light/effects/PraxisEffect.h"
#include "light/effects/FixedRectangleEffect.h"
#include "light/effects/RandomEffect.h"
#include "light/effects/LissajousEffect.h"
#include "light/effects/RubiksCubeEffect.h"
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
#include "light/drivers/LightPresetsModule.h"  // the reusable light-preset library (Drivers submodule)
#include "light/drivers/HueDriver.h"
#include "light/drivers/NetworkSendDriver.h"
#include "light/drivers/NdiDriver.h"
#include "light/drivers/HlsDriver.h"
#include "light/drivers/RtspDriver.h"
#include "light/drivers/PreviewDriver.h"
/// LED drivers are compiled in per chip, gated on the peripheral each one needs, so a board carries only the drivers its silicon can run: @xref{why-led-drivers-are-gated-by-the-preprocessor}.
#if defined(CONFIG_SOC_RMT_SUPPORTED) || MM_LINKS_ALL_LED_DRIVERS
#include "light/drivers/RmtLedDriver.h"
#endif
// The parallel-WS2812 driver + its peripheral backends: @xref{why-led-drivers-are-gated-by-the-preprocessor}.
#if defined(CONFIG_SOC_LCD_I80_SUPPORTED) || MM_LINKS_ALL_LED_DRIVERS
#include "light/drivers/I80Peripheral.h"      // esp_lcd i80 backend (I80Peripheral)
#endif
#if defined(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED) || MM_LINKS_ALL_LED_DRIVERS
#include "light/drivers/MoonI80Peripheral.h"          // MoonI80 own-GDMA backend (MoonI80Peripheral)
#endif
#if defined(CONFIG_SOC_PARLIO_SUPPORTED) || MM_LINKS_ALL_LED_DRIVERS
#include "light/drivers/ParlioPeripheral.h"        // Parlio backend (ParlioPeripheral)
#endif
// Panel receiver cards are opt-in per firmware rather than per chip: @xref{why-panel-cards-are-per-firmware-and-hub75-is-per-chip}.
#if defined(MM_PANEL_CARDS) || MM_LINKS_ALL_LED_DRIVERS
#include "light/drivers/PanelCardDriver.h"
#endif
// HUB75 is a GPIO panel, not a receiver card, so it gates on the chip: @xref{why-panel-cards-are-per-firmware-and-hub75-is-per-chip}.
#if defined(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED) || defined(CONFIG_SOC_PARLIO_SUPPORTED) || \
    MM_LINKS_ALL_LED_DRIVERS
#include "light/drivers/Hub75Driver.h"
#endif
#include "core/system/HttpServerModule.h"
#include "core/system/SystemModule.h"
#include "core/system/ControlModule.h"
#include "core/services/Services.h"
#include "core/services/AudioService.h"
#include "core/services/OscModule.h"
#include "core/system/I2cScanModule.h"
#include "core/system/TasksModule.h"
#include "core/system/PinsModule.h"
#include "core/services/AnalogService.h"
#include "core/services/ButtonService.h"
#include "core/services/InfraredService.h"
#include "core/services/MoonLiveService.h"
#include "core/system/FileManagerModule.h"
#include "core/system/FirmwareUpdateModule.h"
#include "core/system/MoonCloudModule.h"
#include "core/system/MoonStatsModule.h"
#include "core/system/MoonTalkModule.h"
#include "core/system/ImprovProvisioningModule.h"
#include "core/system/MqttModule.h"
#include "core/system/DevicesModule.h"
#include "core/system/FilesystemModule.h"
#include "core/util/ModuleFactory.h"
#include "platform/platform.h"

#include "core/system/NetworkModule.h"

#include <cstdio>

static void registerModuleTypes() {
    // Containers first, the second argument being the module's spec page: @xref{what-registertype-captures}.
    mm::ModuleFactory::registerType<mm::Layouts>("Layouts", "light/supporting.md#layouts");
    mm::ModuleFactory::registerType<mm::Effects>("Effects", "light/supporting.md#effects");
    mm::ModuleFactory::registerType<mm::Layer>("Layer", "light/supporting.md#layer");
    mm::ModuleFactory::registerType<mm::Drivers>("Drivers", "light/supporting.md#drivers");
    mm::ModuleFactory::registerType<mm::LightPresetsModule>("LightPresetsModule", "light/supporting.md#lightpresets");

    // Wire the core quiesce-render hook to the light domain's encode worker: @xref{why-the-quiesce-render-hook-is-a-function-pointer}.
    mm::MoonModule::setQuiesceRenderHook([] { if (auto* d = mm::Drivers::active()) d->quiesceRenderSplit(); });
    // Concrete modules, layouts first, alphabetical by display name: @xref{what-registertype-captures}.
    mm::ModuleFactory::registerType<mm::CarLightsLayout>("CarLightsLayout", "light/layouts.md#carlights");
    mm::ModuleFactory::registerType<mm::CubeLayout>("CubeLayout", "light/layouts.md#cube");
    mm::ModuleFactory::registerType<mm::HumanSizedCubeLayout>("HumanSizedCubeLayout", "light/layouts.md#humansizedcube");
    mm::ModuleFactory::registerType<mm::MoonLiveLayout>("MoonLiveLayout",
                                                        "light/layouts.md#moonlive");
    mm::ModuleFactory::registerType<mm::PanelsLayout>("PanelsLayout", "light/layouts.md#panels");
    mm::ModuleFactory::registerType<mm::TorontoBarGourdsLayout>("TorontoBarGourdsLayout", "light/layouts.md#torontobargourds");
    mm::ModuleFactory::registerType<mm::GridLayout>("GridLayout", "light/layouts.md#grid");
    mm::ModuleFactory::registerType<mm::GridBlacksLayout>("GridBlacksLayout", "light/layouts.md#gridblacks");
    mm::ModuleFactory::registerType<mm::PanelLayout>("PanelLayout", "light/layouts.md#panel");
    mm::ModuleFactory::registerType<mm::RingLayout>("RingLayout", "light/layouts.md#ring");
    mm::ModuleFactory::registerType<mm::Rings241Layout>("Rings241Layout", "light/layouts.md#rings241");
    mm::ModuleFactory::registerType<mm::SingleColumnLayout>("SingleColumnLayout", "light/layouts.md#singlecolumn");
    mm::ModuleFactory::registerType<mm::SingleRowLayout>("SingleRowLayout", "light/layouts.md#singlerow");
    mm::ModuleFactory::registerType<mm::SphereLayout>("SphereLayout", "light/layouts.md#sphere");
    mm::ModuleFactory::registerType<mm::SpiralLayout>("SpiralLayout", "light/layouts.md#spiral");
    mm::ModuleFactory::registerType<mm::TubesLayout>("TubesLayout", "light/layouts.md#tubes");
    mm::ModuleFactory::registerType<mm::WheelLayout>("WheelLayout", "light/layouts.md#wheel");
    // Effects, registered alphabetically by display name: @xref{what-registertype-captures}.
    mm::ModuleFactory::registerType<mm::AudioSpectrumEffect>("AudioSpectrumEffect", "light/effects.md#audiospectrum");
    mm::ModuleFactory::registerType<mm::RadialSpectrumEffect>("RadialSpectrumEffect", "light/effects.md#radialspectrum");
    mm::ModuleFactory::registerType<mm::VuMetersEffect>("VuMetersEffect", "light/effects.md#vumeters");
    mm::ModuleFactory::registerType<mm::BeatRipplesEffect>("BeatRipplesEffect", "light/effects.md#beatripples");
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
    mm::ModuleFactory::registerType<mm::MoonLiveEffect>("MoonLiveEffect", "light/effects.md#moonlive");
    mm::ModuleFactory::registerType<mm::NetworkReceiveEffect>("NetworkReceiveEffect", "light/effects.md#networkreceive");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect", "light/effects.md#noise");
    mm::ModuleFactory::registerType<mm::NoiseMeterEffect>("NoiseMeterEffect", "light/effects.md#noisemeter");
    mm::ModuleFactory::registerType<mm::PaintBrushEffect>("PaintBrushEffect", "light/effects.md#paintbrush");
    mm::ModuleFactory::registerType<mm::ParticlesEffect>("ParticlesEffect", "light/effects.md#particles");
    mm::ModuleFactory::registerType<mm::PlasmaEffect>("PlasmaEffect", "light/effects.md#plasma");
    mm::ModuleFactory::registerType<mm::PraxisEffect>("PraxisEffect", "light/effects.md#praxis");
    mm::ModuleFactory::registerType<mm::PulseEffect>("PulseEffect", "light/effects.md#pulse");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect", "light/effects.md#rainbow");
    mm::ModuleFactory::registerType<mm::RandomEffect>("RandomEffect", "light/effects.md#random");
    mm::ModuleFactory::registerType<mm::RingsEffect>("RingsEffect", "light/effects.md#rings");
    mm::ModuleFactory::registerType<mm::RipplesEffect>("RipplesEffect", "light/effects.md#ripples");
    mm::ModuleFactory::registerType<mm::RubiksCubeEffect>("RubiksCubeEffect", "light/effects.md#rubikscube");
    mm::ModuleFactory::registerType<mm::SineEffect>("SineEffect", "light/effects.md#sine");
    mm::ModuleFactory::registerType<mm::SolidEffect>("SolidEffect", "light/effects.md#solid");
    mm::ModuleFactory::registerType<mm::SdfShapesEffect>("SdfShapesEffect", "light/effects.md#sdfshapes");
    mm::ModuleFactory::registerType<mm::AuroraEffect>("AuroraEffect", "light/effects.md#aurora");
    mm::ModuleFactory::registerType<mm::PolarNoiseEffect>("PolarNoiseEffect", "light/effects.md#polarnoise");
    mm::ModuleFactory::registerType<mm::WaterRippleEffect>("WaterRippleEffect", "light/effects.md#waterripple");
    mm::ModuleFactory::registerType<mm::FluidEffect>("FluidEffect", "light/effects.md#fluid");
    mm::ModuleFactory::registerType<mm::NebulaEffect>("NebulaEffect", "light/effects.md#nebula");
    mm::ModuleFactory::registerType<mm::TrailsEffect>("TrailsEffect", "light/effects.md#trails");
    mm::ModuleFactory::registerType<mm::ColorTrailsEffect>("ColorTrailsEffect", "light/effects.md#colortrails");
    mm::ModuleFactory::registerType<mm::TunnelEffect>("TunnelEffect", "light/effects.md#tunnel");
    mm::ModuleFactory::registerType<mm::EchoEffect>("EchoEffect", "light/effects.md#echo");
    mm::ModuleFactory::registerType<mm::DissolveEffect>("DissolveEffect", "light/effects.md#dissolve");
    mm::ModuleFactory::registerType<mm::SpectrumEffect>("SpectrumEffect", "light/effects.md#spectrum");
    mm::ModuleFactory::registerType<mm::FireworksEffect>("FireworksEffect", "light/effects.md#fireworks");
    mm::ModuleFactory::registerType<mm::FishTankEffect>("FishTankEffect", "light/effects.md#fishtank");
    mm::ModuleFactory::registerType<mm::PacmanEffect>("PacmanEffect", "light/effects.md#pacman");
    mm::ModuleFactory::registerType<mm::FixedPointEffect>("FixedPointEffect", "light/effects.md#fixedpoint");
    mm::ModuleFactory::registerType<mm::MovingHeadEffect>("MovingHeadEffect", "light/effects.md#movinghead");
    mm::ModuleFactory::registerType<mm::FlyingToastersEffect>("FlyingToastersEffect", "light/effects.md#flyingtoasters");
    mm::ModuleFactory::registerType<mm::SpaceInvadersEffect>("SpaceInvadersEffect", "light/effects.md#spaceinvaders");
    mm::ModuleFactory::registerType<mm::SpriteFountainEffect>("SpriteFountainEffect", "light/effects.md#spritefountain");
    mm::ModuleFactory::registerType<mm::PongEffect>("PongEffect", "light/effects.md#pong");
    mm::ModuleFactory::registerType<mm::BallpitEffect>("BallpitEffect", "light/effects.md#ballpit");
    mm::ModuleFactory::registerType<mm::TruchetEffect>("TruchetEffect", "light/effects.md#truchet");
    mm::ModuleFactory::registerType<mm::VectorBallsEffect>("VectorBallsEffect", "light/effects.md#vectorballs");
#if MM_HEAVY_COMPUTE
    // Only where the platform declares per-pixel float headroom; absent entirely elsewhere.
    mm::ModuleFactory::registerType<mm::RaymarchEffect>("RaymarchEffect", "light/effects.md#raymarch");
#endif
    mm::ModuleFactory::registerType<mm::SphereMoveEffect>("SphereMoveEffect", "light/effects.md#spheremove");
    mm::ModuleFactory::registerType<mm::SpiralEffect>("SpiralEffect", "light/effects.md#spiral");
    mm::ModuleFactory::registerType<mm::StarFieldEffect>("StarFieldEffect", "light/effects.md#starfield");
    mm::ModuleFactory::registerType<mm::StarSkyEffect>("StarSkyEffect", "light/effects.md#starsky");
    mm::ModuleFactory::registerType<mm::TetrixEffect>("TetrixEffect", "light/effects.md#tetrix");
    mm::ModuleFactory::registerType<mm::TextEffect>("TextEffect", "light/effects.md#text");
    mm::ModuleFactory::registerType<mm::WaveEffect>("WaveEffect", "light/effects.md#wave");
    // Modifiers, alphabetical by display name.
    mm::ModuleFactory::registerType<mm::BlockModifier>("BlockModifier", "light/modifiers.md#block");
    mm::ModuleFactory::registerType<mm::CheckerboardModifier>("CheckerboardModifier", "light/modifiers.md#checkerboard");
    mm::ModuleFactory::registerType<mm::MoonLiveModifier>("MoonLiveModifier",
                                                          "light/modifiers.md#moonlive");
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
    // NDI is gated by CAPABILITY, not by firmware, and `hasNdi` decides whether the picker offers it: @xref{why-led-drivers-are-gated-by-the-preprocessor}.
    if constexpr (mm::platform::hasNdi)
        mm::ModuleFactory::registerType<mm::NdiDriver>("NdiDriver", "light/drivers.md#ndi");
    if constexpr (mm::platform::hasHls)
        mm::ModuleFactory::registerType<mm::HlsDriver>("HlsDriver", "light/drivers.md#hls");
    if constexpr (mm::platform::hasRtsp)
        mm::ModuleFactory::registerType<mm::RtspDriver>("RtspDriver", "light/drivers.md#rtsp");
    // Same firmware gate as the include above.
#if defined(MM_PANEL_CARDS) || MM_LINKS_ALL_LED_DRIVERS
    mm::ModuleFactory::registerType<mm::PanelCardDriver>("PanelCardDriver", "light/drivers.md#panelcard");
#endif
    // Same silicon gate as the include above.
#if defined(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED) || defined(CONFIG_SOC_PARLIO_SUPPORTED) || \
    MM_LINKS_ALL_LED_DRIVERS
    mm::ModuleFactory::registerType<mm::Hub75Driver>("Hub75Driver", "light/drivers.md#hub75");
#endif
    // Register only the LED drivers this chip's silicon can run, see the gated includes above: @xref{why-led-drivers-are-gated-by-the-preprocessor}.
#if defined(CONFIG_SOC_RMT_SUPPORTED) || MM_LINKS_ALL_LED_DRIVERS
    mm::ModuleFactory::registerType<mm::RmtLedDriver>("RmtLedDriver", "light/drivers.md#rmtled");
#endif
    // One driver for the parallel output whatever the DMA peripheral: each backend self-registers when its header is included, so the control offers exactly the ones this chip links.
#if defined(CONFIG_SOC_LCD_I80_SUPPORTED) || defined(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED) || defined(CONFIG_SOC_PARLIO_SUPPORTED) || MM_LINKS_ALL_LED_DRIVERS
    mm::ModuleFactory::registerType<mm::ParallelLedDriver>("ParallelLedDriver", "light/drivers.md#parallelled");
#endif
    mm::ModuleFactory::registerType<mm::HttpServerModule>("HttpServerModule", "core/system.md");
    mm::ModuleFactory::registerType<mm::SystemModule>("SystemModule", "core/system.md#system");
    mm::ModuleFactory::registerType<mm::ControlModule>("ControlModule", "core/system.md#control");
    mm::ModuleFactory::registerType<mm::Services>("Services", "core/services.md#services");
    mm::ModuleFactory::registerType<mm::AudioService>("AudioService", "core/services.md#audio");
    mm::ModuleFactory::registerType<mm::OscModule>("OscModule", "core/services.md#osc");
    mm::ModuleFactory::registerType<mm::I2cScanModule>("I2cScanModule", "core/system.md#i2c-scan");
    mm::ModuleFactory::registerType<mm::TasksModule>("TasksModule", "core/system.md#tasks");
    mm::ModuleFactory::registerType<mm::PinsModule>("PinsModule", "core/system.md#pins");
    mm::ModuleFactory::registerType<mm::ButtonService>("ButtonService", "core/services.md#button");
    mm::ModuleFactory::registerType<mm::AnalogService>("AnalogService", "core/services.md#analog");
    mm::ModuleFactory::registerType<mm::InfraredService>("InfraredService", "core/services.md#infrared");
    mm::ModuleFactory::registerType<mm::MoonLiveService>("MoonLiveService", "core/services.md#moonliveservice");
    mm::ModuleFactory::registerType<mm::FileManagerModule>("FileManagerModule", "core/system.md#file-manager");
    mm::ModuleFactory::registerType<mm::FirmwareUpdateModule>("FirmwareUpdateModule", "core/system.md#firmware-update");
    mm::ModuleFactory::registerType<mm::MoonCloudModule>("MoonCloudModule", "core/system.md#mooncloud");
    mm::ModuleFactory::registerType<mm::MoonStatsModule>("MoonStatsModule", "core/system.md#stats");
    mm::ModuleFactory::registerType<mm::MoonTalkModule>("MoonTalkModule", "core/system.md#talk");
    mm::ModuleFactory::registerType<mm::ImprovProvisioningModule>("ImprovProvisioningModule", "core/system.md#improv-provisioning");
    mm::ModuleFactory::registerType<mm::MqttModule>("MqttModule", "core/system.md#mqtt");
    mm::ModuleFactory::registerType<mm::DevicesModule>("DevicesModule", "core/system.md#devices");
    mm::ModuleFactory::registerType<mm::NetworkModule>("NetworkModule", "core/system.md#network");
    mm::ModuleFactory::registerType<mm::FilesystemModule>("FilesystemModule", "core/system.md#filesystem");
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

    // Every module below is created via the factory, named by it, and its result deliberately unchecked: @xref{how-the-boot-tree-is-created}.

    // Filesystem (first, wires the load hook into the scheduler so persisted values overlay into other modules' bound variables before their setup() runs)
    auto* filesystemModule = static_cast<mm::FilesystemModule*>(mm::ModuleFactory::create("FilesystemModule"));
    filesystemModule->setScheduler(&scheduler);

    // File Manager, a boot-wired device-wide tool for browsing the filesystem, distinct from FilesystemModule which is the persistence engine: @xref{why-the-device-wide-tools-are-boot-wired}.
    auto* fileManagerModule = static_cast<mm::FileManagerModule*>(mm::ModuleFactory::create("FileManagerModule"));
    fileManagerModule->setName("File Manager");

    // System (deviceName needed by other modules)
    auto* systemModule = static_cast<mm::SystemModule*>(mm::ModuleFactory::create("SystemModule"));
    systemModule->setScheduler(&scheduler);

    // The device's inspection toolkit, wired by code as System children rather than user-added. Always present, exempt from the persistence trim, and accepted by no container as an editable child, so no card offers a delete.
    auto* tasksModule = static_cast<mm::TasksModule*>(mm::ModuleFactory::create("TasksModule"));
    tasksModule->markWiredByCode();
    systemModule->addChild(tasksModule);
    auto* i2cScanModule = static_cast<mm::I2cScanModule*>(mm::ModuleFactory::create("I2cScanModule"));
    i2cScanModule->markWiredByCode();
    systemModule->addChild(i2cScanModule);
    auto* pinsModule = static_cast<mm::PinsModule*>(mm::ModuleFactory::create("PinsModule"));
    pinsModule->markWiredByCode();
    systemModule->addChild(pinsModule);

    // Services, the core-domain twin of Effects/Drivers: a grouping node, added as a root below, whose children the user adds and removes at runtime.
    auto* servicesModule = static_cast<mm::Services*>(mm::ModuleFactory::create("Services"));

    // Boot-wired rather than user-added, because the default effect reacts to sound and a device without this module shows none of it: @xref{why-the-audio-service-is-boot-wired}.
    auto* audioService = static_cast<mm::AudioService*>(mm::ModuleFactory::create("AudioService"));
    audioService->markWiredByCode();   // simulate is the member's own default, so nothing to set
    servicesModule->addChild(audioService);

    // ControlModule puts the device into a named state, top-level because a preset reaches ACROSS Layouts, Effects, Drivers and Services: @xref{why-the-device-wide-tools-are-boot-wired}.
    auto* controlModule = static_cast<mm::ControlModule*>(mm::ModuleFactory::create("ControlModule"));

    // The device identity is SystemModule's own pair of controls rather than a separate module. Tooling injects the model like any catalog default, over HTTP or serial, both routed through the apply core and the control's validator.

    // Surfaces the install's status as read-only controls, polling the shared globals so the push picks up progress while HTTP drives the flash itself. Renamed because the card hosts the install picker.
    auto* firmwareUpdateModule = static_cast<mm::FirmwareUpdateModule*>(
        mm::ModuleFactory::create("FirmwareUpdateModule"));
    firmwareUpdateModule->setName("Firmware");

    // The container for everything talking to a server we run, each child carrying its own consent: @xref{why-mooncloud-is-not-a-firmware-child}.
    auto* moonCloudModule = static_cast<mm::MoonCloudModule*>(
        mm::ModuleFactory::create("MoonCloudModule"));
    moonCloudModule->setName("MoonCloud");

    auto* moonStatsModule = static_cast<mm::MoonStatsModule*>(
        mm::ModuleFactory::create("MoonStatsModule"));
    moonStatsModule->setName("Stats");

    // MoonTalk, the public message board and a SECOND MoonCloud child with its own consent: @xref{why-mooncloud-is-not-a-firmware-child}.
    auto* moonTalkModule = static_cast<mm::MoonTalkModule*>(
        mm::ModuleFactory::create("MoonTalkModule"));
    moonTalkModule->setName("Talk");

    // Network (platform stubs return false on desktop, module is a no-op)
    auto* networkModule = static_cast<mm::NetworkModule*>(mm::ModuleFactory::create("NetworkModule"));
    networkModule->setScheduler(&scheduler);
    networkModule->setSystemModule(systemModule);

    // Listens on the serial port for pushed WiFi credentials, compile-time gated: @xref{why-improv-is-compile-time-gated}.
    mm::ImprovProvisioningModule* improvModule = nullptr;
    if constexpr (mm::platform::hasImprov) {
        improvModule = static_cast<mm::ImprovProvisioningModule*>(
            mm::ModuleFactory::create("ImprovProvisioningModule"));
        improvModule->setSystemModule(systemModule);
        improvModule->setNetworkModule(networkModule);
        // Marked wired-by-code so the trim loop preserves it on a device whose saved tree predates this child: @xref{why-markwiredbycode-matters}.
        improvModule->markWiredByCode();
    }

    // MQTT service, a code-wired child of Network bridging the light controls to a broker: @xref{why-mqtt-is-built-on-every-networked-target}.
    mm::MqttModule* mqttModule = nullptr;
    if constexpr (mm::platform::hasNetwork) {
        mqttModule = static_cast<mm::MqttModule*>(mm::ModuleFactory::create("MqttModule"));
        mqttModule->setSystemModule(systemModule);
        mqttModule->setControlModule(controlModule);   // look-only presets as the HA effect list
        mqttModule->markWiredByCode();
    }

    // Layouts, the top-level container for one or more layouts, today one GridLayout that self-initializes to defaultGridSize with no boot-time dimensions threaded in.
    auto* layouts = static_cast<mm::Layouts*>(mm::ModuleFactory::create("Layouts"));
    auto* grid = static_cast<mm::GridLayout*>(mm::ModuleFactory::create("GridLayout"));
    layouts->addChild(grid);

    // Effects: top-level container; one or more layers, each rendering into its own buffer. Today one Layer with one effect + one modifier.
    auto* effectsContainer = static_cast<mm::Effects*>(mm::ModuleFactory::create("Effects"));
    auto* layer = static_cast<mm::Layer*>(mm::ModuleFactory::create("Layer"));
    layer->setChannelsPerLight(3);
    effectsContainer->addChild(layer);
    // setLayouts wires the shared Layouts to the container AND propagates to every child Layer.
    effectsContainer->setLayouts(layouts);

    // One default effect so a bare device still shows lights out of the box, but NO default modifier: @xref{why-the-boot-layer-is-one-pulse-effect}.
    auto* pulse = mm::ModuleFactory::create("PulseEffect");
    layer->addChild(pulse);

    // Bound to the effects container rather than to a single layer. A layer rebuilt through the API self-heals without re-running this wiring, and one driver can read across several layer buffers from one place.
    auto* drivers = static_cast<mm::Drivers*>(mm::ModuleFactory::create("Drivers"));
    drivers->setEffects(effectsContainer);

    // Output drivers are added per board through the catalog rather than boot-wired, the preview being the one exception: @xref{why-output-drivers-are-not-boot-wired}.

    // The preset library, a boot-wired singleton owning the named channel-role wirings every driver references by id, resolved through its own seat since exactly one exists.
    auto* lightPresets =
        static_cast<mm::LightPresetsModule*>(mm::ModuleFactory::create("LightPresetsModule"));
    drivers->addChild(lightPresets);
    lightPresets->markWiredByCode();

    auto* preview = static_cast<mm::PreviewDriver*>(mm::ModuleFactory::create("PreviewDriver"));
    drivers->addChild(preview);
    // Marked wired-by-code, the same protection ImprovProvisioning uses: @xref{why-markwiredbycode-matters}.
    preview->markWiredByCode();

    auto* httpServer = static_cast<mm::HttpServerModule*>(mm::ModuleFactory::create("HttpServerModule"));
    httpServer->port = httpPort;
    httpServer->setScheduler(&scheduler);
    // PreviewDriver pushes the coordinate table and per-frame RGB to the HTTP server's WS broadcaster: @xref{why-output-drivers-are-not-boot-wired}.
    preview->setBroadcaster(httpServer);

    // The APPLY_OP vendor RPC, wired here once httpServer exists: @xref{why-improv-is-compile-time-gated}.
    if (improvModule) improvModule->setHttpServerModule(httpServer);

    // Registration order matters, and the scheduler walks the roots in it each tick: @xref{why-registration-order-matters}.
    scheduler.addModule(filesystemModule);
    scheduler.addModule(systemModule);
    scheduler.addModule(fileManagerModule);
    scheduler.addModule(firmwareUpdateModule);
    // Both are boot wiring, not user-added, so the persisted tree must not decide whether they exist: @xref{why-markwiredbycode-matters}.
    moonStatsModule->markWiredByCode(); moonCloudModule->addChild(moonStatsModule);
    moonTalkModule->markWiredByCode();  moonCloudModule->addChild(moonTalkModule);
    scheduler.addModule(moonCloudModule);
    if (improvModule) networkModule->addChild(improvModule);
    if (mqttModule) networkModule->addChild(mqttModule);
    // Devices discovers other devices on the LAN, a Network child since discovery depends on the network being up, and wired-by-code (see DevicesModule.md).
    auto* devicesModule = static_cast<mm::DevicesModule*>(
        mm::ModuleFactory::create("DevicesModule"));
    devicesModule->markWiredByCode();
    // Wire our own name so the self row in the device list matches the device's identity elsewhere. deviceName has static lifetime as SystemModule's member, so the module borrows the pointer.
    devicesModule->setSelfName(systemModule->deviceName());
    networkModule->addChild(devicesModule);
    scheduler.addModule(networkModule);
    scheduler.addModule(servicesModule);
    scheduler.addModule(controlModule);
    scheduler.addModule(layouts);
    scheduler.addModule(effectsContainer);
    scheduler.addModule(drivers);
    // Only where an IP stack exists, which is what makes a network-less build supported: @xref{why-registration-order-matters}.
    if constexpr (mm::platform::hasNetwork) scheduler.addModule(httpServer);

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
    // The server binds every interface, so it is reachable across the LAN: @xref{what-the-boot-banner-prints}.
    const char* hostIp = mm::platform::hostIp();
    if (hostIp && hostIp[0]) {
        std::printf("HTTP server → http://%s:%u\n", hostIp, httpServer->port);
    } else {
        // No address yet, which is true for two different reasons: @xref{what-the-boot-banner-prints}.
        std::printf("HTTP server on port %u — no network address yet\n", httpServer->port);
    }

    size_t heap = mm::platform::freeHeap();
    if (heap > 0) {
        std::printf("Free heap: %u bytes\n", static_cast<unsigned>(heap));
    }
    std::fflush(stdout);

    uint32_t lastLog = mm::platform::millis();
    const uint32_t bootMillis = lastLog;   // window start for the MM_IP serial token
    bool mmIpWindowClosed = false;         // latches true once the 60 s window elapses

    // Subscribe THIS render-loop task to the task watchdog, so a genuine wedge here panics and reboots instead of hanging silently: @xref{why-the-render-loop-subscribes-to-the-task-watchdog}.
    mm::platform::taskWdtSubscribe();

    while (keepRunning) {
        scheduler.tick();
        mm::platform::taskWdtReset();   // feed the render-loop WDT subscription — a live tick is not a wedge

        // Log every second
        uint32_t now = mm::platform::millis();
        if (now - lastLog >= 1000) {
            lastLog = now;
            // `goto`, not `continue`, because the loop's pacing lives at its TAIL: @xref{why-the-loop-pacing-uses-goto-and-a-sleep}.
            if (scheduler.tickTimeUs() == 0) goto paced; // no measurement yet

            // A plain write rather than a log call, gating on the log level by hand: @xref{why-the-periodic-line-is-a-plain-write}.
            const bool inBootWindow = !mmIpWindowClosed && (now - bootMillis < 60000);
            if (systemModule->logLevel() < mm::platform::LogLevel::Info && !inBootWindow) goto paced;

            heap = mm::platform::freeHeap();
            std::printf("tick: %uus (FPS: %u)", static_cast<unsigned>(scheduler.tickTimeUs()),
                        static_cast<unsigned>(scheduler.fps()));
            if (heap > 0) {
                // maxInternalAllocBlock, internal RAM only: @xref{why-the-periodic-line-is-a-plain-write}.
                std::printf("  free: %u  maxBlock: %u",
                            static_cast<unsigned>(heap),
                            static_cast<unsigned>(mm::platform::maxInternalAllocBlock()));
            }
            // The worst wait at a frame boundary in the last second, not a single frame's: @xref{why-the-periodic-line-is-a-plain-write}.
            if (drivers->renderSplitActive())
                std::printf("  renderWait: %uus", static_cast<unsigned>(drivers->renderWaitPeakUs()));
            // A stable address token for the installer's post-flash serial read, gated to the first minute: @xref{why-the-address-token-rides-the-periodic-line}.
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
                        // The discovery name alongside the address, so the installer's link survives a lease change: @xref{why-the-address-token-rides-the-periodic-line}.
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

    paced:
        // Pace the loop rather than spin, since a bare yield burns a whole core: @xref{why-the-loop-pacing-uses-goto-and-a-sleep}.
        mm::platform::yield();
        mm::platform::pauseLoop();
    }

    std::printf("\nShutting down.\n");
    scheduler.release();
}
