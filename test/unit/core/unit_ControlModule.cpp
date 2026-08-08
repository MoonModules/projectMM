// @module ControlModule
// @also FilesystemModule, Scheduler

// Presets end to end: a preset is a file, saving writes one, selecting reads it back. These pin the
// behaviour a user would describe — save a look, change it, get it back — plus the ways a preset
// file can be wrong, since a device that loses its state to a bad file is worse than one with no
// presets at all.

#include "doctest.h"
#include "core/ControlModule.h"
#include "core/FilesystemModule.h"
#include "core/ModuleFactory.h"
#include "core/Scheduler.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "light/drivers/Drivers.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

// A device with Effects and a ControlModule, built the way production builds one: through the
// factory, with an isolated filesystem so the assertions do not depend on what is on this machine.
struct Device {
    mm::Scheduler scheduler;
    mm::FilesystemModule* fs = nullptr;
    mm::ControlModule* control = nullptr;
    mm::MoonModule* layers = nullptr;
    mm::MoonModule* drivers = nullptr;
    char root_[256] = {};

    Device() {
        // A monotonic counter, not millis(): two fixtures constructed in the same millisecond would
        // otherwise share a root and see each other's preset files.
        static unsigned seq = 0;
        std::snprintf(root_, sizeof(root_), "/tmp/mm_preset_test_%u", ++seq);
        std::filesystem::remove_all(root_);
        mm::platform::fsSetRoot(root_);

        mm::ModuleFactory::registerType<mm::Effects>("Effects");
        mm::ModuleFactory::registerType<mm::Layer>("Layer");
        mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
        mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
        mm::ModuleFactory::registerType<mm::ControlModule>("ControlModule");
        mm::ModuleFactory::registerType<mm::Drivers>("Drivers");

        fs = new mm::FilesystemModule();
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        layers = mm::ModuleFactory::create("Effects");
        drivers = mm::ModuleFactory::create("Drivers");
        control = static_cast<mm::ControlModule*>(mm::ModuleFactory::create("ControlModule"));
        scheduler.addModule(fs);
        scheduler.addModule(layers);
        scheduler.addModule(drivers);
        scheduler.addModule(control);
        scheduler.setup();
    }

    ~Device() { std::filesystem::remove_all(root_); }   // don't leave a directory per test behind

    mm::MoonModule* add(mm::MoonModule* parent, const char* type) {
        auto* m = mm::ModuleFactory::create(type);
        REQUIRE(m != nullptr);
        parent->addChild(m);
        m->defineControls();
        m->setup();
        return m;
    }

    /// Set a control's text value. The change hook is NOT fired here — a test that needs it calls
    /// press() explicitly, which is what the UI does as a second step.
    void setText(const char* controlName, const char* value) {
        auto& cs = control->controls();
        for (uint8_t i = 0; i < cs.count(); i++) {
            if (std::strcmp(cs[i].name, controlName) != 0) continue;
            std::snprintf(static_cast<char*>(cs[i].ptr), static_cast<size_t>(cs[i].max), "%s", value);
            return;
        }
        FAIL("no control named ", controlName);
    }

    /// What the next save captures: exactly one of Layouts / Effects / Drivers / Services.
    void setCapture(const char* typeName) {
        auto& cs = control->controls();
        for (uint8_t i = 0; i < cs.count(); i++) {
            if (std::strcmp(cs[i].name, "captures") != 0) continue;
            for (uint8_t r = 0; r < mm::ControlModule::kCaptureCount; r++) {
                if (std::strcmp(mm::ControlModule::kCapturable[r], typeName) != 0) continue;
                *static_cast<uint8_t*>(cs[i].ptr) = r;
                return;
            }
            FAIL("no capturable type named ", typeName);
        }
        FAIL("no captures control");
    }

    void press(const char* button) { control->onControlChanged(button); }

    /// The row JSON for a preset by name, which is how the pad grid is inspected.
    std::string rowNamed(const char* name) const {
        for (uint8_t i = 0; i < control->listRowCount(); i++) {
            mm::JsonSink sink;
            control->writeListRow(sink, i);
            std::string row(sink.data(), sink.size());
            if (row.find(std::string("\"name\":\"") + name + "\"") != std::string::npos) return row;
        }
        return "";
    }

    /// Apply a preset by name, the way a pad click does.
    void activate(const char* name) {
        const std::string row = rowNamed(name);
        REQUIRE_MESSAGE(!row.empty(), "no preset named ", name);
        const uint32_t id = static_cast<uint32_t>(std::stoul(row.substr(row.find("\"id\":") + 5)));
        control->setListRowField(id, "activate", "{}");
    }

    /// The module's own status, the one every module reports through MoonModule::setStatus and the
    /// UI shows in the card's status chip.
    const char* status() const { return control->status() ? control->status() : ""; }

    const char* effectType() const {
        auto* layer = layers->child(0);
        if (!layer) return "";
        auto* fx = layer->child(0);
        return fx ? fx->typeName() : "";
    }

    /// The stable id of the one preset row, or 0 when the list is empty.
    uint32_t firstRowId() const {
        if (control->listRowCount() == 0) return 0;
        mm::JsonSink sink;
        control->writeListRow(sink, 0);
        const std::string row(sink.data(), sink.size());
        const size_t at = row.find("\"id\":");
        return at == std::string::npos ? 0 : static_cast<uint32_t>(std::stoul(row.substr(at + 5)));
    }
};

}  // namespace

// The feature in one test: keep a look, change it, bring it back.
TEST_CASE("ControlModule saves a look and puts it back") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setText("name", "sunset");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 1);

    // The user moves on to something else.
    auto* old = layer->replaceChildAt(0, mm::ModuleFactory::create("RainbowEffect"));
    if (old) { old->release(); mm::Scheduler::deleteTree(old); }
    REQUIRE(std::strcmp(d.effectType(), "RainbowEffect") == 0);

    d.control->setListRowField(d.firstRowId(), "apply", "{}");
    CHECK(std::strcmp(d.effectType(), "NoiseEffect") == 0);
}

// A preset is a file, so the folder is the list: what is on disk is what the user sees.
TEST_CASE("ControlModule lists one row per preset file") {
    Device d;
    d.add(d.layers, "Layer");

    d.setText("name", "one");
    d.press("save");
    d.setText("name", "two");
    d.press("save");

    CHECK(d.control->listRowCount() == 2);
}

// Deleting a preset deletes its file. Nothing else holds preset state, so there is no second copy
// that could disagree with the folder.
TEST_CASE("ControlModule deletes a preset by deleting its file") {
    Device d;
    d.add(d.layers, "Layer");
    d.setText("name", "throwaway");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 1);

    CHECK(d.control->deleteListRow(d.firstRowId()));
    CHECK(d.control->listRowCount() == 0);
}

// Saving without a name would write ".json" and produce a nameless row, so it is refused with a
// message rather than silently creating something the user cannot identify.
TEST_CASE("ControlModule refuses to save a preset with no name") {
    Device d;
    d.add(d.layers, "Layer");

    d.press("save");
    CHECK(d.control->listRowCount() == 0);
    CHECK(std::string(d.status()).find("name") != std::string::npos);
}

// A preset carries exactly ONE role, so a file written by an older build that names several is
// listed but not applied: applying it would do something other than what its name suggests. It stays
// on the grid (and on disk) so it can be seen and deleted, rather than silently disappearing.
TEST_CASE("ControlModule refuses to apply a preset carrying several roles") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    const char* body =
        "{\"captures\":\"Layouts,Effects\","
        "\"Effects.enabled\":true,"
        "\"Effects.0.type\":\"Layer\",\"Effects.0.enabled\":true}";
    mm::platform::fsMkdir(mm::ControlModule::kPresetDir);
    char path[160];
    std::snprintf(path, sizeof(path), "%s/legacy.json", mm::ControlModule::kPresetDir);
    REQUIRE(mm::platform::fsWriteAtomic(path, body, std::strlen(body)));
    d.control->setup();

    REQUIRE(d.control->listRowCount() == 1);                 // visible, so it can be deleted
    CHECK(d.control->roleOf(0) == mm::ControlModule::kCaptureCount);   // but it holds no single role
    CHECK_FALSE(d.control->setListRowField(d.firstRowId(), "apply", "{}"));
    CHECK(std::string(d.status()).find("several roles") != std::string::npos);
}

// A preset carrying a module this build does not have applies what it can and says what it skipped:
// a preset from another board must degrade rather than refuse or crash.
TEST_CASE("ControlModule refuses a preset whose subtree this build does not have") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    // Hand-written: names a single module type no build registers, so the apply must refuse with a
    // reason rather than report success for a preset that changed nothing.
    const std::string body =
        "{\"captures\":\"NoSuchTopLevelXyz\","
        "\"Effects.enabled\":true,"
        "\"Effects.0.type\":\"Layer\",\"Effects.0.enabled\":true,"
        "\"Effects.0.0.type\":\"NoiseEffect\",\"Effects.0.0.enabled\":true}";
    mm::platform::fsMkdir(mm::ControlModule::kPresetDir);
    char path[128];
    std::snprintf(path, sizeof(path), "%s/mixed.json", mm::ControlModule::kPresetDir);
    REQUIRE(mm::platform::fsWriteAtomic(path, body.c_str(), body.size()));

    d.control->setup();                      // rescan picks the file up
    REQUIRE(d.control->listRowCount() == 1);

    CHECK_FALSE(d.control->setListRowField(d.firstRowId(), "apply", "{}"));
    CHECK(std::string(d.status()).find("nothing this build knows") != std::string::npos);
}

// A truncated file (an interrupted upload) must leave the device with the look it already had.
TEST_CASE("ControlModule survives a corrupt preset file") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    mm::platform::fsMkdir(mm::ControlModule::kPresetDir);
    char path[128];
    std::snprintf(path, sizeof(path), "%s/broken.json", mm::ControlModule::kPresetDir);
    const char* truncated = "{\"captures\":\"Effects\",\"Effects.0.ty";
    REQUIRE(mm::platform::fsWriteAtomic(path, truncated, std::strlen(truncated)));

    d.control->setup();
    REQUIRE(d.control->listRowCount() == 1);                         // the file IS listed
    // Apply returns false: the row exists and was attempted, but nothing in it was usable. Asserting
    // the return as well as the tree is what separates "rejected the bad file" from "silently did
    // nothing at all", which an unchanged tree alone cannot tell apart.
    CHECK_FALSE(d.control->setListRowField(d.firstRowId(), "apply", "{}"));

    CHECK(d.layers->childCount() == 1);                              // the tree is untouched
    CHECK(std::strcmp(d.effectType(), "NoiseEffect") == 0);
}

// The row says what the preset carries, so a user can tell a portable look from a device snapshot
// before applying it.
TEST_CASE("ControlModule shows what each preset captures") {
    Device d;
    d.add(d.layers, "Layer");
    d.setText("name", "look");
    d.press("save");

    mm::JsonSink sink;
    d.control->writeListRow(sink, 0);
    const std::string row(sink.data(), sink.size());
    CHECK(row.find("\"name\":\"look\"") != std::string::npos);
    CHECK(row.find("Effects") != std::string::npos);
}

// The pad grid answers "what is on right now" without a click, so the applied preset marks itself
// active and any other preset does not.
TEST_CASE("ControlModule marks the applied preset as the active pad") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setText("name", "first");
    d.press("save");
    d.setText("name", "second");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 2);

    auto rowJson = [&](uint8_t i) {
        mm::JsonSink sink;
        d.control->writeListRow(sink, i);
        return std::string(sink.data(), sink.size());
    };
    // Nothing applied yet: no pad is lit.
    CHECK(rowJson(0).find("\"active\":true") == std::string::npos);
    CHECK(rowJson(1).find("\"active\":true") == std::string::npos);

    // Apply one, and exactly that one lights up.
    mm::JsonSink idSink;
    d.control->writeListRow(idSink, 0);
    const std::string first(idSink.data(), idSink.size());
    const uint32_t id = static_cast<uint32_t>(std::stoul(first.substr(first.find("\"id\":") + 5)));
    d.control->setListRowField(id, "activate", "{}");

    CHECK(rowJson(0).find("\"active\":true") != std::string::npos);
    CHECK(rowJson(1).find("\"active\":true") == std::string::npos);
}

// A pad click and a row button are the same action from two presentations, so both reach apply.
TEST_CASE("ControlModule applies a preset from either the pad or the row button") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");
    d.setText("name", "look");
    d.press("save");

    auto swapToRainbow = [&] {
        auto* old = layer->replaceChildAt(0, mm::ModuleFactory::create("RainbowEffect"));
        if (old) { old->release(); mm::Scheduler::deleteTree(old); }
    };

    swapToRainbow();
    CHECK(d.control->setListRowField(d.firstRowId(), "activate", "{}"));   // the pad
    CHECK(std::strcmp(d.effectType(), "NoiseEffect") == 0);

    swapToRainbow();
    CHECK(d.control->setListRowField(d.firstRowId(), "apply", "{}"));      // the row button
    CHECK(std::strcmp(d.effectType(), "NoiseEffect") == 0);
}

// Pads can be dragged into the order the user wants, so a grid can be arranged to match a physical
// control surface, and that arrangement survives a rescan — the filesystem's own file order is not
// the order anyone chose.
TEST_CASE("ControlModule keeps the pad order the user arranged") {
    Device d;
    d.add(d.layers, "Layer");
    for (const char* n : {"a", "b", "c"}) { d.setText("name", n); d.press("save"); }
    REQUIRE(d.control->listRowCount() == 3);

    auto nameAt = [&](uint8_t i) {
        mm::JsonSink sink;
        d.control->writeListRow(sink, i);
        const std::string row(sink.data(), sink.size());
        const size_t at = row.find("\"name\":\"") + 8;
        return row.substr(at, row.find('"', at) - at);
    };
    auto idAt = [&](uint8_t i) {
        mm::JsonSink sink;
        d.control->writeListRow(sink, i);
        const std::string row(sink.data(), sink.size());
        return static_cast<uint32_t>(std::stoul(row.substr(row.find("\"id\":") + 5)));
    };

    // Move the last pad to the front, as a drag would.
    const std::string last = nameAt(2);
    REQUIRE(d.control->moveListRow(idAt(2), 0));
    CHECK(nameAt(0) == last);

    // And it is still first after a rescan, because the order lives in the files.
    d.control->setup();
    CHECK(nameAt(0) == last);
}

// A preset says which roles it covers, so a pad can show the same emoji the module cards use and a
// user can tell a portable look from one that carries the hardware.
TEST_CASE("ControlModule reports the roles a preset covers") {
    Device d;
    d.add(d.layers, "Layer");
    d.setText("name", "look");
    d.press("save");

    mm::JsonSink sink;
    d.control->writeListRow(sink, 0);
    const std::string row(sink.data(), sink.size());
    CHECK(row.find("\"roles\":[\"effects\"]") != std::string::npos);   // Effects is captured by default
}

// Fader 1 rides the global brightness every driver scales by, through the same setControl primitive
// IR and the network bridges use — so a hardware surface bound to it later drives the device the
// same way the on-screen fader does.
TEST_CASE("ControlModule fader 1 drives the global brightness") {
    Device d;
    auto brightness = [&] {
        auto& cs = d.drivers->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, "brightness") == 0) return *static_cast<uint8_t*>(cs[i].ptr);
        return static_cast<uint8_t>(0);
    };
    const uint8_t before = brightness();

    // Move the fader the way the UI does: set its value, then fire its change hook.
    auto& cs = d.control->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "fader1") != 0) continue;
        *static_cast<uint8_t*>(cs[i].ptr) = 200;
        d.control->onControlChanged("fader1");
        break;
    }
    CHECK(brightness() == 200);
    CHECK(before != 200);   // the value really moved, rather than already being there
}

// The unassigned faders have no target yet, so moving one must be inert rather than driving
// something by accident.
TEST_CASE("ControlModule an unassigned fader drives nothing") {
    Device d;
    auto brightness = [&] {
        auto& cs = d.drivers->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, "brightness") == 0) return *static_cast<uint8_t*>(cs[i].ptr);
        return static_cast<uint8_t>(0);
    };
    const uint8_t before = brightness();

    auto& cs = d.control->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "fader3") != 0) continue;
        *static_cast<uint8_t*>(cs[i].ptr) = 99;
        d.control->onControlChanged("fader3");
        break;
    }
    CHECK(brightness() == before);
}

// A save lands on the pad the user right-clicked, not in the first free cell: on a surface, WHERE
// something goes is the user's choice, and a save that ignored it would scatter presets.
TEST_CASE("ControlModule saves a preset onto the chosen pad") {
    Device d;
    d.add(d.layers, "Layer");

    auto setU8 = [&](const char* name, uint8_t v) {
        auto& cs = d.control->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, name) == 0) { *static_cast<uint8_t*>(cs[i].ptr) = v; return; }
        FAIL("no control named ", name);
    };

    d.setText("name", "corner");
    setU8("slot", 20);                 // a pad well away from the first free cell
    d.press("save");

    mm::JsonSink sink;
    d.control->writeListRow(sink, 0);
    const std::string row(sink.data(), sink.size());
    CHECK(row.find("\"slot\":20") != std::string::npos);
}

// The encoder row exists and is unassigned: the affordance ships before the binding UI, so an
// encoder must be inert rather than driving something by accident.
TEST_CASE("ControlModule has an encoder row that drives nothing yet") {
    Device d;
    auto brightness = [&] {
        auto& cs = d.drivers->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, "brightness") == 0) return *static_cast<uint8_t*>(cs[i].ptr);
        return static_cast<uint8_t>(0);
    };
    const uint8_t before = brightness();

    auto& cs = d.control->controls();
    uint8_t found = 0;
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strncmp(cs[i].name, "enc", 3) != 0) continue;
        found++;
        *static_cast<uint8_t*>(cs[i].ptr) = 123;
        d.control->onControlChanged(cs[i].name);
    }
    CHECK(found == mm::ControlModule::kEncoderCount);
    CHECK(brightness() == before);     // nothing was driven
}


// Presets hold their roles independently, which is what lets a surface show a layout choice and a
// look choice lit at the same time. Applying a layer preset replaces the layer holder and leaves the
// layout one alone, so both pads stay lit and the grid says what is on across all four roles.
TEST_CASE("ControlModule keeps one active preset per captured role") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    // A driver-only preset and a layer-only preset: two roles, two pads.
    d.setCapture("Drivers");
    d.setText("name", "geometry");
    d.press("save");

    d.setCapture("Effects");
    d.setText("name", "look");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 2);

    d.activate("geometry");
    CHECK(d.rowNamed("geometry").find("\"active\":true") != std::string::npos);

    // The layer preset claims only the layer role, so the driver pad stays lit.
    d.activate("look");
    CHECK(d.rowNamed("look").find("\"active\":true") != std::string::npos);
    CHECK(d.rowNamed("geometry").find("\"active\":true") != std::string::npos);

    // And each reports WHICH role it holds, which is what the pad colors itself by.
    CHECK(d.rowNamed("geometry").find("\"activeRoles\":[\"driver\"]") != std::string::npos);
    CHECK(d.rowNamed("look").find("\"activeRoles\":[\"effects\"]") != std::string::npos);
}

// Each role is held independently, so applying a look replaces the look and leaves the geometry
// alone. With one role per preset this is the whole supersede rule.
TEST_CASE("ControlModule replaces only the role a preset carries") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setCapture("Drivers");
    d.setText("name", "hardware");  d.press("save");
    d.setCapture("Effects");
    d.setText("name", "lookA");     d.press("save");
    d.setText("name", "lookB");     d.press("save");

    d.activate("hardware");
    d.activate("lookA");
    CHECK(d.rowNamed("hardware").find("\"active\":true") != std::string::npos);
    CHECK(d.rowNamed("lookA").find("\"active\":true") != std::string::npos);

    // A second look takes the layer role from the first; the driver preset is untouched.
    d.activate("lookB");
    CHECK(d.rowNamed("lookB").find("\"active\":true") != std::string::npos);
    CHECK(d.rowNamed("lookA").find("\"active\":true") == std::string::npos);
    CHECK(d.rowNamed("hardware").find("\"active\":true") != std::string::npos);
}


// Renaming a preset renames its file, so the pad keeps its contents under the new name. The name IS
// the identity here (there is no id inside the file), which is what makes a rename a file move
// rather than an edit.
TEST_CASE("ControlModule renames a preset by renaming its file") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setText("name", "before");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 1);

    REQUIRE(d.control->setListRowField(d.firstRowId(), "name", "{\"value\":\"after\"}"));
    REQUIRE(d.control->listRowCount() == 1);                  // still one preset, not two
    CHECK(!d.rowNamed("after").empty());
    CHECK(d.rowNamed("before").empty());

    // The renamed preset still applies, so the rename moved the contents rather than just the label.
    auto* old = layer->replaceChildAt(0, mm::ModuleFactory::create("RainbowEffect"));
    if (old) { old->release(); mm::Scheduler::deleteTree(old); }
    REQUIRE(std::strcmp(d.effectType(), "RainbowEffect") == 0);
    d.activate("after");
    CHECK(std::strcmp(d.effectType(), "NoiseEffect") == 0);
}

// A preset that captures nothing the device has still leaves a usable surface: the row exists, and
// applying it reports failure rather than silently claiming success. Pins the distinction the status
// line depends on — "applied" and "nothing applied" must not look the same to the caller.
TEST_CASE("ControlModule reports an apply that changed nothing") {
    Device d;
    d.add(d.layers, "Layer");

    d.setCapture("Effects");
    d.setText("name", "look");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 1);

    // A file whose captures name a subtree this build does not have.
    char path[160];
    std::snprintf(path, sizeof(path), "%s/ghost.json", mm::ControlModule::kPresetDir);
    const char* body = "{\"captures\":\"NoSuchModuleXyz\",\"NoSuchModuleXyz.enabled\":true}";
    REQUIRE(mm::platform::fsWriteAtomic(path, body, std::strlen(body)));
    d.control->setup();
    REQUIRE(d.control->listRowCount() == 2);

    const std::string row = d.rowNamed("ghost");
    REQUIRE(!row.empty());
    const uint32_t id = static_cast<uint32_t>(std::stoul(row.substr(row.find("\"id\":") + 5)));
    CHECK_FALSE(d.control->setListRowField(id, "activate", "{}"));   // nothing applied, and it says so
}


// A preset name becomes a file name, so it must not be able to steer the path out of the preset
// folder. ESP32's filesystem layer does no path normalization (the desktop one does), so a name
// carrying `..` would escape on device while looking clean on a developer's machine — and delete and
// rename write through the same path. The name control's validator is what closes that.
TEST_CASE("ControlModule refuses a preset name that could escape its folder") {
    Device d;
    d.add(d.layers, "Layer");

    auto* nameCtrl = [&]() -> const mm::ControlDescriptor* {
        auto& cs = d.control->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, "name") == 0) return &cs[i];
        return nullptr;
    }();
    REQUIRE(nameCtrl != nullptr);
    REQUIRE(nameCtrl->validate != nullptr);          // the guard is bound, not just written

    // Anything that could steer a path is refused...
    CHECK_FALSE(nameCtrl->validate("../escape"));
    CHECK_FALSE(nameCtrl->validate("../../etc/passwd"));
    CHECK_FALSE(nameCtrl->validate("sub/dir"));
    CHECK_FALSE(nameCtrl->validate("back\\slash"));
    CHECK_FALSE(nameCtrl->validate("dot.name"));     // the .json suffix is ours to add
    CHECK_FALSE(nameCtrl->validate(""));

    // ...while ordinary names a user would pick still work.
    CHECK(nameCtrl->validate("sunset"));
    CHECK(nameCtrl->validate("Warm White 2"));

    // And a rename cannot smuggle one in through the row-edit path either.
    d.setText("name", "safe");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 1);
    CHECK_FALSE(d.control->setListRowField(d.firstRowId(), "name", "{\"value\":\"../escape\"}"));
    CHECK(!d.rowNamed("safe").empty());              // the preset is untouched
}


// Renaming onto a name that already exists must refuse rather than overwrite: the write would clobber
// the other preset and the follow-up remove would delete the source, losing a preset the user never
// named. Same refuse-on-collision stance the pad move takes.
TEST_CASE("ControlModule refuses to rename a preset over an existing one") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setText("name", "keep");
    d.press("save");
    d.setText("name", "other");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 2);

    const std::string row = d.rowNamed("other");
    REQUIRE(!row.empty());
    const uint32_t id = static_cast<uint32_t>(std::stoul(row.substr(row.find("\"id\":") + 5)));

    CHECK_FALSE(d.control->setListRowField(id, "name", "{\"value\":\"keep\"}"));
    CHECK(d.control->listRowCount() == 2);         // both survive
    CHECK(!d.rowNamed("keep").empty());
    CHECK(!d.rowNamed("other").empty());
}


// The saved file must be valid JSON, not merely readable by our own first-match key helpers: a
// preset is downloaded, edited and re-uploaded by users and tools. The separator between the header
// and each namespaced subtree is easy to get wrong in a way our lenient reader would not notice.
TEST_CASE("ControlModule writes a preset that is well-formed JSON") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setCapture("Effects");
    d.setText("name", "wellformed");
    d.press("save");

    char path[160];
    std::snprintf(path, sizeof(path), "%s/wellformed.json", mm::ControlModule::kPresetDir);
    const long size = mm::platform::fsSize(path);
    REQUIRE(size > 0);
    std::string body(static_cast<size_t>(size) + 1, '\0');
    REQUIRE(mm::platform::fsRead(path, body.data(), body.size()) > 0);
    body.resize(std::strlen(body.c_str()));

    // A minimal structural check: balanced braces, no empty pair, and no doubled separator -- the
    // three ways a hand-assembled object breaks.
    CHECK(body.front() == '{');
    CHECK(body.back() == '}');
    CHECK(body.find(",,") == std::string::npos);
    CHECK(body.find("{,") == std::string::npos);
    CHECK(body.find(",}") == std::string::npos);
    int depth = 0;
    for (char c : body) { if (c == '{') depth++; else if (c == '}') depth--; }
    CHECK(depth == 0);
}


// An applied preset must SURVIVE a reboot. Applying rebuilds the live tree, but the boot loader
// restores from the config file -- so without marking the tree dirty the device renders the preset
// now and comes back to the previous look after a restart, which reads as "the preset did not save".
TEST_CASE("ControlModule persists the look a preset applied") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setCapture("Effects");
    d.setText("name", "keeper");
    d.press("save");

    // Change the look, and let that change reach the config file.
    auto* old = layer->replaceChildAt(0, mm::ModuleFactory::create("RainbowEffect"));
    if (old) { old->release(); mm::Scheduler::deleteTree(old); }
    d.fs->flush();
    REQUIRE(std::strcmp(d.effectType(), "RainbowEffect") == 0);

    d.activate("keeper");
    REQUIRE(std::strcmp(d.effectType(), "NoiseEffect") == 0);   // applied to the LIVE tree

    // The config file must now describe the applied look, not the one it replaced.
    d.fs->flush();
    char path[160];
    std::snprintf(path, sizeof(path), "/.config/%s.json", d.layers->typeName());
    const long size = mm::platform::fsSize(path);
    REQUIRE(size > 0);
    std::string body(static_cast<size_t>(size) + 1, '\0');
    REQUIRE(mm::platform::fsRead(path, body.data(), body.size()) > 0);
    body.resize(std::strlen(body.c_str()));
    CHECK(body.find("NoiseEffect") != std::string::npos);
    CHECK(body.find("RainbowEffect") == std::string::npos);
}


// Only a pure look may be reachable from outside. A preset that also carries Drivers or Layouts
// rewires pins or geometry, and an external surface (a voice assistant, an automation) must not be
// able to do that while it thinks it is picking a colour scheme.
TEST_CASE("ControlModule exposes only look-only presets to external surfaces") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setCapture("Effects");
    d.setText("name", "purelook");  d.press("save");

    d.setCapture("Drivers");
    d.setText("name", "pinsonly");  d.press("save");
    REQUIRE(d.control->presetCount() == 2);

    auto lookOnlyNamed = [&](const char* n) {
        for (uint8_t i = 0; i < d.control->presetCount(); i++)
            if (std::strcmp(d.control->presetName(i), n) == 0) return d.control->isLookOnly(i);
        FAIL("no preset named ", n);
        return false;
    };
    CHECK(lookOnlyNamed("purelook"));
    CHECK_FALSE(lookOnlyNamed("pinsonly"));    // hardware, not a look

    // And the apply entry point enforces it, not just the listing: naming a hardware-carrying preset
    // through the external path is refused outright.
    CHECK(d.control->applyLookByName("purelook"));
    CHECK_FALSE(d.control->applyLookByName("pinsonly"));
    CHECK_FALSE(d.control->applyLookByName("nosuchpreset"));

    CHECK(std::string(d.control->currentLook()) == "purelook");
}


// The Home Assistant effect list is sized from what the device actually has, not from a fixed cap: a
// cap would either reserve RAM a small setup never uses, or silently publish nothing once the list
// outgrew it (a truncated config is refused, never sent, so the entity would just vanish).
TEST_CASE("ControlModule sizes the Home Assistant look list to the presets that exist") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    auto lookNames = [&]() {
        size_t bytes = 0, count = 0;
        for (uint8_t i = 0; i < d.control->presetCount(); i++) {
            if (!d.control->isLookOnly(i)) continue;
            bytes += std::strlen(d.control->presetName(i));
            count++;
        }
        return std::make_pair(count, bytes);
    };

    // Nothing to publish yet.
    CHECK(lookNames().first == 0);

    d.setCapture("Effects");
    d.setText("name", "one");       d.press("save");
    const auto afterOne = lookNames();
    CHECK(afterOne.first == 1);

    d.setText("name", "twotwotwo"); d.press("save");
    const auto afterTwo = lookNames();
    CHECK(afterTwo.first == 2);
    CHECK(afterTwo.second > afterOne.second);      // the requirement GREW with the longer name

    // A hardware-carrying preset must not enlarge the list at all: it is never published.
    d.setCapture("Drivers");
    d.setText("name", "hardware");  d.press("save");
    CHECK(lookNames().first == 2);
    CHECK(lookNames().second == afterTwo.second);
}


// Home Assistant caches the preset list and only re-fetches when the device's reported revision
// changes. A value that can stand still across a mutation means a preset saved, renamed or deleted
// after HA set the device up never appears in its dropdown — the endpoint stays correct while HA
// shows a stale copy forever. The revision is a COUNTER, not a timestamp, precisely so two
// mutations inside the same second still read as two changes — which also makes this test
// deterministic, with no clock involved.
TEST_CASE("ControlModule bumps its revision on every preset-set change") {
    Device d;
    d.add(d.layers, "Layer");

    d.setCapture("Effects");
    d.setText("name", "first");
    d.press("save");
    const uint32_t afterFirst = d.control->presetsRevision();
    CHECK(afterFirst > 0);                  // setup's rescan already counts as revision 1

    // Two mutations back-to-back — same second, and each must still read as a change.
    d.setText("name", "second");
    d.press("save");
    const uint32_t afterSecond = d.control->presetsRevision();
    CHECK(afterSecond > afterFirst);

    REQUIRE(d.control->deleteListRow(d.firstRowId()));
    CHECK(d.control->presetsRevision() > afterSecond);
}


// A pad holds one preset. Saving a DIFFERENT name onto an occupied pad would leave two files
// claiming the same cell, of which the grid can render only one — so the save refuses and says who
// holds the pad. Saving the SAME name on its own pad is the normal save-over flow and still works.
TEST_CASE("ControlModule refuses to save a new preset onto an occupied pad") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    auto setU8 = [&](const char* name, uint8_t v) {
        auto& cs = d.control->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, name) == 0) { *static_cast<uint8_t*>(cs[i].ptr) = v; return; }
        FAIL("no control named ", name);
    };

    d.setCapture("Effects");
    d.setText("name", "holder");
    setU8("slot", 5);
    d.press("save");
    REQUIRE(d.control->listRowCount() == 1);

    // A different name aimed at the same pad: refused, nothing new created.
    d.setText("name", "intruder");
    setU8("slot", 5);
    d.press("save");
    CHECK(d.control->listRowCount() == 1);
    CHECK(std::string(d.status()).find("taken") != std::string::npos);

    // The holder itself saving onto its own pad is the overwrite flow.
    d.setText("name", "holder");
    setU8("slot", 5);
    d.press("save");
    CHECK(d.control->listRowCount() == 1);
    CHECK(!d.rowNamed("holder").empty());
}


// A pad must go dark when its preset is deleted. The active-role slots refer to a preset BY NAME, so
// without clearing them the grid keeps lighting a pad for a file that no longer exists — and a new
// preset saved under the reused name would inherit the lit state.
TEST_CASE("ControlModule stops showing a deleted preset as active") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setCapture("Effects");
    d.setText("name", "doomed");
    d.press("save");
    d.activate("doomed");
    REQUIRE(std::string(d.control->currentLook()) == "doomed");

    REQUIRE(d.control->deleteListRow(d.firstRowId()));
    CHECK(std::string(d.control->currentLook()).empty());
}

// A renamed preset keeps its lit pad: the active-role slot follows the new name rather than pointing
// at a name that no longer exists.
TEST_CASE("ControlModule keeps a renamed preset active under its new name") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setCapture("Effects");
    d.setText("name", "before");
    d.press("save");
    d.activate("before");
    REQUIRE(std::string(d.control->currentLook()) == "before");

    REQUIRE(d.control->setListRowField(d.firstRowId(), "name", "{\"value\":\"after\"}"));
    CHECK(std::string(d.control->currentLook()) == "after");
}
