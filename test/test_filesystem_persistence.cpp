#include "doctest.h"
#include "core/FilesystemModule.h"
#include "core/ModuleFactory.h"
#include "core/Scheduler.h"
#include "core/SystemModule.h"
#include "light/NoiseEffect.h"
#include "light/RainbowEffect.h"
#include "light/MirrorModifier.h"
#include "light/Layer.h"
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

        // FilesystemModule debounces saves by 2s after the dirty mark. We can't sleep
        // (well, we can — and we do) — busy-poll loop1s() until the file appears or
        // a 3-second wall-time deadline expires.
        uint32_t deadline = mm::platform::millis() + 3000;
        while (mm::platform::millis() < deadline) {
            fs->loop1s();
            char path[256];
            std::snprintf(path, sizeof(path), "%s/.config/SystemModule.json", tmpRoot);
            if (std::filesystem::exists(path)) break;
        }

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
