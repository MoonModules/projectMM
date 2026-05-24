#include "doctest.h"
#include "core/FilesystemModule.h"
#include "core/ModuleFactory.h"
#include "core/Scheduler.h"
#include "core/SystemModule.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/modifiers/MirrorModifier.h"
#include "light/layers/Layer.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

// Persistence round-trip: set deviceName → save → recreate Scheduler+modules → load → assert.
// Uses fsSetRoot to isolate the test from any real /.config/ on disk.
TEST_CASE("FilesystemModule round-trip") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_persist_test_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(tmpRoot);

    // Scheduler::teardown() deletes its modules — they must be heap-allocated.
    // --- First run: set deviceName, save ---
    {
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        auto* sys = new mm::SystemModule();
        sys->setTypeName("SystemModule");
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        sys->setScheduler(&scheduler);
        scheduler.addModule(fs);
        scheduler.addModule(sys);
        scheduler.setup();

        // setup() derived MAC-based default; mutate it as if the UI did:
        for (uint8_t i = 0; i < sys->controls().count(); i++) {
            auto& c = sys->controls()[i];
            if (std::strcmp(c.name, "deviceName") == 0) {
                std::strncpy(static_cast<char*>(c.ptr), "MM-ROUND", 8);
                static_cast<char*>(c.ptr)[8] = 0;
                sys->markDirty();
                mm::FilesystemModule::noteDirty();
                break;
            }
        }

        // flush() does the same work as loop1s() does once the debounce expires, but
        // synchronously — used here to keep the test deterministic without wall-clock waits.
        fs->flush();

        char path[256];
        std::snprintf(path, sizeof(path), "%s/.config/SystemModule.json", tmpRoot);
        CHECK(std::filesystem::exists(path));

        scheduler.teardown();
    }

    // --- Second run: fresh modules, load from disk ---
    {
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        auto* sys = new mm::SystemModule();
        sys->setTypeName("SystemModule");
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        sys->setScheduler(&scheduler);
        scheduler.addModule(fs);
        scheduler.addModule(sys);
        scheduler.setup();

        CHECK(std::strcmp(sys->deviceName(), "MM-ROUND") == 0);

        scheduler.teardown();
    }

    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot("."); // restore default
}

// Structural persistence: hand-write a Layer.json describing a different tree shape
// than the one main.cpp builds, then load and verify the live tree reconciles to
// match the JSON — type swap at position 0, trim of position 1.
TEST_CASE("FilesystemModule structural reconciliation") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_struct_test_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");
    mm::platform::fsSetRoot(tmpRoot);

    // ModuleFactory must know the types before reconciliation can construct them.
    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::MirrorModifier>("MirrorModifier");

    // Write a Layer.json that asks for one child (RainbowEffect at position 0).
    {
        std::ofstream f(std::string(tmpRoot) + "/.config/Layer.json");
        f << "{\"channelsPerLight\":3,\"enabled\":true,"
             "\"0.type\":\"RainbowEffect\",\"0.enabled\":true}";
    }

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);

    // Build a live tree: Layer with NoiseEffect at pos 0 and MirrorModifier at pos 1.
    // The JSON wants RainbowEffect at pos 0 and nothing at pos 1 — so we expect a swap
    // and a trim.
    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    auto* noise = new mm::NoiseEffect();
    noise->setTypeName("NoiseEffect");
    auto* mirror = new mm::MirrorModifier();
    mirror->setTypeName("MirrorModifier");
    layer->addChild(noise);
    layer->addChild(mirror);

    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    REQUIRE(layer->childCount() == 1);
    CHECK(std::strcmp(layer->child(0)->typeName(), "RainbowEffect") == 0);

    scheduler.teardown();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Round-trip persistence with children: write a Layer subtree that contains both
// controls and child modules with controls of their own, then read the file back as
// text and verify it parses as valid JSON. Regresses the missing-comma bug between
// each child's "N.type" field and that child's first control (e.g. "0.type":"X""0.foo":1
// instead of "0.type":"X","0.foo":1).
TEST_CASE("FilesystemModule writes valid JSON with children") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_write_test_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");
    mm::platform::fsSetRoot(tmpRoot);

    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
    mm::ModuleFactory::registerType<mm::MirrorModifier>("MirrorModifier");

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);
    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    auto* mirror = new mm::MirrorModifier();
    mirror->setTypeName("MirrorModifier");
    auto* noise = new mm::NoiseEffect();
    noise->setTypeName("NoiseEffect");
    layer->addChild(mirror);
    layer->addChild(noise);

    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // Mark dirty and flush so the file appears immediately.
    layer->markDirty();
    fs->flush();

    // Read back the raw file and verify both child "type" fields are followed by
    // a comma before the next field — the previously-broken serializer emitted
    // "0.type":"MirrorModifier""0.mirrorX":true with no separator.
    std::ifstream f(std::string(tmpRoot) + "/.config/Layer.json");
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(content.find("\"MirrorModifier\",") != std::string::npos);
    CHECK(content.find("\"NoiseEffect\",") != std::string::npos);
    // And the catastrophic "}{ or "X""Y syntactic shape must not appear.
    CHECK(content.find("\"\"") == std::string::npos);

    scheduler.teardown();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Singleton survives probe lifecycle: /api/types factory-creates a probe of every
// registered type (including FilesystemModule) to capture defaults, then deletes it.
// The probe's destructor must NOT clear the singleton — otherwise every save path
// (noteDirty, debounced loop1s, flushPending on reboot) silently no-ops for the rest
// of the device's life. The fix is to register the singleton in setScheduler(), not
// in the constructor. This test would have caught the bug if it had existed before.
TEST_CASE("FilesystemModule singleton survives probe construct+destruct") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_singleton_test_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");
    mm::platform::fsSetRoot(tmpRoot);

    mm::ModuleFactory::registerType<mm::FilesystemModule>("FilesystemModule");
    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");

    mm::Scheduler scheduler;

    // 1) Real FS instance, registered via setScheduler (this is the singleton-binding path).
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);

    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // 2) Mimic /api/types: factory-construct a probe FilesystemModule, then delete it.
    //    Before the fix, the probe's destructor cleared the static singleton because
    //    `instance_ == this` for the probe at destruction time.
    {
        auto* probe = mm::ModuleFactory::create("FilesystemModule");
        REQUIRE(probe != nullptr);
        delete probe;
    }

    // 3) After the probe died, noteDirty() must still reach the real singleton. We
    //    verify it indirectly: mark Layer dirty + flush, and observe that the Layer.json
    //    file appears on disk. If the singleton was lost, flush would be a no-op
    //    (flushPending() returns early when instance_ is null) and the file would not
    //    exist.
    layer->markDirty();
    mm::FilesystemModule::flushPending();

    std::ifstream f(std::string(tmpRoot) + "/.config/Layer.json");
    CHECK(f.is_open());

    scheduler.teardown();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Regression: Int16 controls (GridLayout's width/height/depth, Layer's start/end)
// round-tripped through the filesystem load path were clamped to c.min/c.max,
// which default to 0,0 because ControlDescriptor.min/max are uint8_t and can't
// represent an int16 range. Every Int16 control loaded as 0 — so a 128×128 grid
// became 0×0×0 after restart and the whole pipeline allocated no buffers.
TEST_CASE("FilesystemModule Int16 controls round-trip preserves the saved value") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_int16_test_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");
    mm::platform::fsSetRoot(tmpRoot);

    // Hand-write a Layer.json with non-zero Int16 values so the load path is
    // exercised without needing a save-side step.
    std::ofstream out(std::string(tmpRoot) + "/.config/Layer.json");
    out << "{\"enabled\":true,\"startX\":42,\"startY\":-17,\"startZ\":0,"
        << "\"endX\":100,\"endY\":-100,\"endZ\":0}";
    out.close();

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);
    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    CHECK(layer->startX == 42);
    CHECK(layer->startY == -17);
    CHECK(layer->endX == 100);
    CHECK(layer->endY == -100);

    scheduler.teardown();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}
