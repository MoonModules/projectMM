#include "doctest.h"
#include "core/Scheduler.h"
#include "core/MoonModule.h"

// Pins Scheduler::ensureUniqueName and deduplicateNamesInTree. These prevent
// the "second Layer (or any same-named module) is unreachable via parent_id"
// bug — the HTTP API uses module names as identifiers, and findModuleByName
// returns the first DFS match, so a second module with the same name from
// ModuleFactory::create (which strips role-noun suffixes and can produce
// duplicates like "Layer") could never be addressed.
//
// Both code paths that introduce modules — HttpServerModule::handleAddModule
// (live add via /api/modules) and the persistence load in Scheduler::setup
// (positional rebuild from /.config/<TypeName>.json) — call into the same
// Scheduler helpers, so a single test covers both.

namespace {

struct Stub : public mm::MoonModule {
    // No role needed for these tests; default Generic is fine.
};

} // namespace

TEST_CASE("Scheduler::ensureUniqueName leaves unique names alone") {
    mm::Scheduler s;
    auto* a = new Stub();
    a->setName("OnlyOne");
    s.addModule(a);

    s.ensureUniqueName(a);
    CHECK(std::strcmp(a->name(), "OnlyOne") == 0);

    s.deleteTree(a);
}

TEST_CASE("Scheduler::ensureUniqueName suffixes the second occurrence") {
    // Build: Container → child A ("Layer"), child B ("Layer"). The second one
    // must be renamed; the first stays.
    mm::Scheduler s;
    auto* parent = new Stub();
    parent->setName("Layers");
    s.addModule(parent);

    auto* first = new Stub();
    first->setName("Layer");
    parent->addChild(first);

    auto* second = new Stub();
    second->setName("Layer");
    parent->addChild(second);

    s.ensureUniqueName(second);
    CHECK(std::strcmp(first->name(), "Layer") == 0);
    CHECK(std::strcmp(second->name(), "Layer 2") == 0);

    s.deleteTree(parent);
}

TEST_CASE("Scheduler::ensureUniqueName keeps suffixing past 'Foo 2'") {
    mm::Scheduler s;
    auto* parent = new Stub();
    parent->setName("Layers");
    s.addModule(parent);

    auto* a = new Stub(); a->setName("Layer");   parent->addChild(a);
    auto* b = new Stub(); b->setName("Layer 2"); parent->addChild(b);
    auto* c = new Stub(); c->setName("Layer");   parent->addChild(c);

    s.ensureUniqueName(c);
    // First "Layer" intact, hand-named "Layer 2" intact, new collision lands at "Layer 3".
    CHECK(std::strcmp(a->name(), "Layer") == 0);
    CHECK(std::strcmp(b->name(), "Layer 2") == 0);
    CHECK(std::strcmp(c->name(), "Layer 3") == 0);

    s.deleteTree(parent);
}

TEST_CASE("Scheduler::deduplicateNamesInTree walks the whole tree") {
    // Simulates the persistence-load case: positional rebuild produced two
    // Layer modules with the same factory default name; one whole-tree pass
    // must disambiguate every duplicate.
    mm::Scheduler s;
    auto* layers = new Stub();
    layers->setName("Layers");
    s.addModule(layers);

    auto* layerA = new Stub();
    layerA->setName("Layer");
    layers->addChild(layerA);

    auto* effectA1 = new Stub();
    effectA1->setName("Noise");
    layerA->addChild(effectA1);

    auto* layerB = new Stub();
    layerB->setName("Layer");      // collides with layerA
    layers->addChild(layerB);

    auto* effectB1 = new Stub();
    effectB1->setName("Noise");    // collides with effectA1
    layerB->addChild(effectB1);

    s.deduplicateNamesInTree();

    // First occurrences keep their names; second ones get " 2" suffix.
    CHECK(std::strcmp(layerA->name(), "Layer") == 0);
    CHECK(std::strcmp(layerB->name(), "Layer 2") == 0);
    CHECK(std::strcmp(effectA1->name(), "Noise") == 0);
    CHECK(std::strcmp(effectB1->name(), "Noise 2") == 0);

    s.deleteTree(layers);
}

TEST_CASE("Scheduler::firstByName returns the first match in tree-walk order") {
    mm::Scheduler s;
    auto* p = new Stub(); p->setName("Layers");      s.addModule(p);
    auto* a = new Stub(); a->setName("Layer");       p->addChild(a);
    auto* b = new Stub(); b->setName("Other");       p->addChild(b);

    CHECK(s.firstByName("Layers") == p);
    CHECK(s.firstByName("Layer")  == a);
    CHECK(s.firstByName("Other")  == b);
    CHECK(s.firstByName("Missing") == nullptr);

    s.deleteTree(p);
}

TEST_CASE("Scheduler::ensureUniqueName leaves the colliding name alone when the suffixed result wouldn't fit") {
    // MoonModule::name_ is 16 bytes (15 chars + NUL). A 13-char base name like
    // "GlowParticles" reaches the buffer ceiling at suffix "10" — "GlowParticles 10"
    // is 16 chars, doesn't fit. ensureUniqueName must refuse to truncate
    // (keep the duplicate, return) rather than silently produce a different
    // result than the caller asked for. This test pins that refusal.
    mm::Scheduler s;
    auto* parent = new Stub();
    parent->setName("Layers");
    s.addModule(parent);

    // Create one base "GlowParticles" plus eight "GlowParticles 2".."GlowParticles 9".
    // The next ensureUniqueName needs "GlowParticles 10" which doesn't fit.
    auto* first = new Stub();
    first->setName("GlowParticles");
    parent->addChild(first);

    for (int n = 2; n <= 9; n++) {
        auto* m = new Stub();
        char nm[16];
        std::snprintf(nm, sizeof(nm), "GlowParticles %d", n);
        m->setName(nm);
        parent->addChild(m);
    }

    // The 10th sibling — ensureUniqueName cannot produce a fitting unique name.
    auto* victim = new Stub();
    victim->setName("GlowParticles");
    parent->addChild(victim);

    s.ensureUniqueName(victim);

    // Refused: keeps "GlowParticles" rather than producing a truncated/colliding name.
    // (Note: this leaves two children both named "GlowParticles". The framework
    // ships with this as a known sharp edge — see ensureUniqueName comment.)
    CHECK(std::strcmp(victim->name(), "GlowParticles") == 0);

    s.deleteTree(parent);
}
