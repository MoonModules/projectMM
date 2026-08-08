// @module FilesystemModule
// @also Scheduler, Layer, ControlModule

// The save/apply seams a preset is built on: `saveSubtreeTo` writes a subtree into a caller's buffer
// rather than to /.config/<TypeName>.json, and `applySubtree` puts those bytes back onto a LIVE tree
// at runtime, rebuilding whatever shape they describe.
//
// Boot already does both halves, but only as one fused operation at startup. What these pin is the
// runtime behaviour a preset needs and boot never exercises: applying onto a tree that is already
// set up, with children that must be created, replaced or destroyed while the device runs.

#include "doctest.h"
#include "core/FilesystemModule.h"
#include "core/ModuleFactory.h"
#include "core/Scheduler.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

// A Effects tree the same way production builds one: through the factory, so every module carries a
// real typeName(). That matters here rather than being ceremony — reconciliation matches children BY
// TYPE, and a module constructed with `new` has an empty type name that can never match.
//
// Children are added AFTER scheduler.setup(), because the boot load trims a live tree down to what
// its saved file describes: with no file, anything added before setup is deleted during it.
struct Tree {
    mm::Scheduler scheduler;
    mm::FilesystemModule* fs = nullptr;
    mm::MoonModule* layers = nullptr;
    char root_[256] = {};

    Tree() {
        // Isolate the filesystem: without this the boot load reads the developer's real
        // /.config/Effects.json and the tree arrives with whatever that machine happened to have,
        // so the assertions below would depend on the box the tests run on.
        // A monotonic counter, not millis(): two fixtures built in the same millisecond would share
        // a root and read each other's files.
        static unsigned seq = 0;
        std::snprintf(root_, sizeof(root_), "/tmp/mm_subtree_test_%u", ++seq);
        std::filesystem::remove_all(root_);
        mm::platform::fsSetRoot(root_);

        mm::ModuleFactory::registerType<mm::Effects>("Effects");
        mm::ModuleFactory::registerType<mm::Layer>("Layer");
        mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
        mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");

        fs = new mm::FilesystemModule();
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        layers = mm::ModuleFactory::create("Effects");
        scheduler.addModule(fs);
        scheduler.addModule(layers);
        scheduler.setup();
    }

    ~Tree() { std::filesystem::remove_all(root_); }   // don't leave a directory per test behind

    /// Add a child at runtime, driving the lifecycle the caller owns (MoonModule.h's contract).
    mm::MoonModule* add(mm::MoonModule* parent, const char* type) {
        auto* m = mm::ModuleFactory::create(type);
        REQUIRE(m != nullptr);
        parent->addChild(m);
        m->defineControls();
        m->setup();
        return m;
    }

    // The effect type at Effects→Layer[0]→child[0], or "" when there is none.
    const char* effectType() const {
        auto* layer = layers->child(0);
        if (!layer) return "";
        auto* fx = layer->child(0);
        return fx ? fx->typeName() : "";
    }
};

std::string serialize(mm::FilesystemModule* fs, mm::MoonModule* m) {
    mm::JsonSink sink;
    REQUIRE(fs->saveSubtreeTo(m, sink));
    return std::string(sink.data(), sink.size());
}

}  // namespace

// A subtree serializes into a caller's buffer, so a preset file can hold the same bytes the
// persistence engine writes rather than needing a second serializer that could drift from it.
TEST_CASE("saveSubtreeTo writes a subtree a caller can keep") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");

    const std::string json = serialize(t.fs, t.layers);

    // The flat dotted-key form the loader reads back: positional child types, enabled per node.
    CHECK(json.find("\"0.type\":\"Layer\"") != std::string::npos);
    CHECK(json.find("\"0.0.type\":\"NoiseEffect\"") != std::string::npos);
    CHECK(json.find("\"enabled\":") != std::string::npos);
}

// The round trip a preset IS: capture a tree, change it live, put the capture back. This is the
// whole feature in one assertion.
TEST_CASE("applySubtree restores a tree that changed since it was captured") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");
    REQUIRE(std::strcmp(t.effectType(), "NoiseEffect") == 0);

    const std::string preset = serialize(t.fs, t.layers);

    // The user changes the look: a different effect entirely.
    auto* old = layer->replaceChildAt(0, mm::ModuleFactory::create("RainbowEffect"));
    if (old) { old->release(); mm::Scheduler::deleteTree(old); }
    REQUIRE(std::strcmp(t.effectType(), "RainbowEffect") == 0);

    CHECK(t.fs->applySubtree(t.layers, preset.c_str()));
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);   // the captured look is back
}

// A preset that carries MORE than the device has must add what is missing: applying it on a tree
// whose children were deleted rebuilds them, which is what makes a preset a restore rather than a
// value overlay.
TEST_CASE("applySubtree recreates children the live tree no longer has") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");

    const std::string preset = serialize(t.fs, t.layers);

    // Everything under Effects is removed, as a user clearing their setup would.
    auto* gone = t.layers->child(0);
    t.layers->removeChild(gone);
    gone->release();
    mm::Scheduler::deleteTree(gone);
    REQUIRE(t.layers->childCount() == 0);

    CHECK(t.fs->applySubtree(t.layers, preset.c_str()));
    REQUIRE(t.layers->childCount() == 1);
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);
}

// And the reverse: a preset captured from a smaller tree must REMOVE what it does not describe, or
// applying it would leave the previous look layered underneath.
TEST_CASE("applySubtree removes children the preset does not describe") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");

    const std::string preset = serialize(t.fs, t.layers);   // captured with an EMPTY layer

    t.add(layer, "NoiseEffect");
    REQUIRE(layer->childCount() == 1);

    CHECK(t.fs->applySubtree(t.layers, preset.c_str()));
    CHECK(layer->childCount() == 0);
}

// A module type this build does not have is skipped and the rest of the preset still applies: a
// preset from a newer firmware, or from a board with a driver this one lacks, must degrade rather
// than refuse to load. Same tolerance the boot loader already has.
TEST_CASE("applySubtree skips an unknown module type and applies the rest") {
    Tree t;

    // Hand-written: a Layer whose first child is a type no factory knows.
    const char* preset =
        "{\"enabled\":true,"
        "\"0.type\":\"Layer\",\"0.enabled\":true,"
        "\"0.0.type\":\"NoSuchEffectXyz\",\"0.0.enabled\":true,"
        "\"0.1.type\":\"NoiseEffect\",\"0.1.enabled\":true}";

    CHECK(t.fs->applySubtree(t.layers, preset));

    REQUIRE(t.layers->childCount() == 1);                       // the Layer applied
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);      // and so did the effect after it
}

// Malformed JSON leaves the tree alone rather than half-applying or crashing. A truncated or
// corrupted preset file is the realistic case (an interrupted upload), and the device must survive
// it with the look it already had.
TEST_CASE("applySubtree survives a corrupt preset") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");

    // Rejected, and it SAYS so: a caller reporting success to a user must be able to tell a real
    // apply from a body that was never credible.
    CHECK_FALSE(t.fs->applySubtree(t.layers, "{\"0.type\":\"Lay"));   // truncated mid-key
    CHECK(t.layers->childCount() == 1);
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);

    CHECK_FALSE(t.fs->applySubtree(t.layers, ""));                    // and an empty body
    CHECK(t.layers->childCount() == 1);
}

// Several subtrees share one flat object, each under its own "<TypeName>." prefix, and each reads
// back independently. This is the shape a preset file uses: it is what lets one file carry a
// selectable set of captures without a nested-object parser.
TEST_CASE("a prefixed subtree round-trips inside a larger object") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");

    mm::JsonSink sink;
    sink.append("{\"captures\":\"Effects\",");
    REQUIRE(t.fs->saveSubtreeTo(t.layers, sink, "Effects."));
    sink.append("}");
    const std::string preset(sink.data(), sink.size());

    CHECK(preset.find("\"Effects.0.type\":\"Layer\"") != std::string::npos);
    CHECK(preset.find("\"Effects.0.0.type\":\"NoiseEffect\"") != std::string::npos);

    // Change the look, then restore it from the prefixed body.
    auto* old = layer->replaceChildAt(0, mm::ModuleFactory::create("RainbowEffect"));
    if (old) { old->release(); mm::Scheduler::deleteTree(old); }
    REQUIRE(std::strcmp(t.effectType(), "RainbowEffect") == 0);

    CHECK(t.fs->applySubtree(t.layers, preset.c_str(), "Effects."));
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);
}
