// @module HttpServerModule

#include "doctest.h"
#include "core/HttpServerModule.h"
#include "core/Scheduler.h"
#include "core/ModuleFactory.h"
#include "core/MoonModule.h"

#include <cstring>

// Pins the transport-free apply-core that HttpServerModule exposes — applyAddModule
// / applySetControl / applyClearChildren / applyOp. These are the operations the
// HTTP /api/modules + /api/control handlers do, factored out of the TcpConnection so
// BOTH the HTTP path and the Improv-serial APPLY_OP path drive one shared
// implementation ("Improv = REST over serial"). Testing them directly here, without
// a socket, is the unit-test win of the extraction: the apply logic is now provable
// in isolation. Also exercises the robustness rule (the apply-core tolerates bad
// input — unknown module, unknown type, malformed op — without crashing, returning a
// typed result instead).

namespace {

// A leaf with one editable Uint8 control + a child-accepting container, so we can
// add, set, and clear-children without pulling in real light modules.
struct Knob : public mm::MoonModule {
    uint8_t value = 10;
    void onBuildControls() override { controls_.addUint8("value", value, 0, 100); }
};
struct Box : public mm::MoonModule {
    // accepts any child (the HTTP role gate lives above the apply-core).
};

// A leaf with a VALIDATED Text control — mirrors SystemModule.deviceModel: the printable-
// ASCII rule is a per-control validator, so a bad value is rejected on EVERY write path
// (including the APPLY_OP `set` the installer uses), not in a bespoke per-transport RPC.
struct Tag : public mm::MoonModule {
    char label[32] = "init";
    static bool printableAscii(const char* v) {
        if (!v) return false;
        size_t n = std::strlen(v);
        if (n == 0 || n >= 32) return false;
        for (size_t i = 0; i < n; i++) {
            unsigned char b = static_cast<unsigned char>(v[i]);
            if (b < 0x20 || b > 0x7E) return false;
        }
        return true;
    }
    void onBuildControls() override {
        controls_.addText("label", label, sizeof(label), printableAscii);
    }
};

// Build a tree: scheduler root "Root" (a Box) with HttpServerModule wired to it.
// Returns via out-params so each case starts clean. Caller owns teardown via the
// scheduler.
void registerTestTypes() {
    static bool done = false;
    if (done) return;
    mm::ModuleFactory::registerType<Knob>("Knob");
    mm::ModuleFactory::registerType<Box>("Box");
    mm::ModuleFactory::registerType<Tag>("Tag");
    done = true;
}

// Find a direct child of `parent` by name (the test inspects the tree directly
// rather than through HttpServerModule's private findModuleByName).
mm::MoonModule* childNamed(mm::MoonModule* parent, const char* name) {
    for (uint8_t i = 0; i < parent->childCount(); i++) {
        auto* c = parent->child(i);
        if (c && std::strcmp(c->name(), name) == 0) return c;
    }
    return nullptr;
}

} // namespace

TEST_CASE("apply-core: applyAddModule adds a child, idempotent on the id") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box();
    root->setName("Root");
    s.addModule(root);
    mm::HttpServerModule http;
    http.setScheduler(&s);

    using OpResult = mm::HttpServerModule::OpResult;

    // Add a Knob named "K" under "Root".
    CHECK(http.applyAddModule("Knob", "K", "Root") == OpResult::Ok);
    CHECK(childNamed(root, "K") != nullptr);

    // Idempotent: re-adding the same id is AlreadyExists (no duplicate) — a distinct
    // success the HTTP handler reports as {"ok":true,"note":"already exists"}.
    CHECK(http.applyAddModule("Knob", "K", "Root") == OpResult::AlreadyExists);
    CHECK(root->childCount() == 1);

    // Unknown type / missing parent / top-level add are typed failures, not crashes.
    CHECK(http.applyAddModule("NopeType", "X", "Root") == OpResult::UnknownType);
    CHECK(http.applyAddModule("Knob", "Y", "NoSuchParent") == OpResult::ModuleNotFound);
    CHECK(http.applyAddModule("Knob", "Z", "") == OpResult::BadRequest);  // no parent → top-level

    s.deleteTree(root);
}

TEST_CASE("apply-core: applySetControl writes a value, rejects out-of-range / unknown") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box();
    root->setName("Root");
    s.addModule(root);
    mm::HttpServerModule http;
    http.setScheduler(&s);
    using OpResult = mm::HttpServerModule::OpResult;

    REQUIRE(http.applyAddModule("Knob", "K", "Root") == OpResult::Ok);

    // The value JSON is the same {"value":N} body the HTTP handler reads by key.
    CHECK(http.applySetControl("K", "value", "{\"value\":42}") == OpResult::Ok);
    auto* k = static_cast<Knob*>(childNamed(root, "K"));
    REQUIRE(k != nullptr);
    CHECK(k->value == 42);

    // Out of the 0..100 range → typed rejection, value left unchanged.
    CHECK(http.applySetControl("K", "value", "{\"value\":999}") == OpResult::OutOfRange);
    CHECK(k->value == 42);

    // Unknown module vs unknown control → distinct typed failures (each a 404 with
    // its own body on the HTTP path), no crash.
    CHECK(http.applySetControl("Nope", "value", "{\"value\":1}") == OpResult::ModuleNotFound);
    CHECK(http.applySetControl("K", "nope", "{\"value\":1}") == OpResult::ControlNotFound);

    s.deleteTree(root);
}

TEST_CASE("apply-core: applyClearChildren empties a container (replaceChildren)") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box();
    root->setName("Root");
    s.addModule(root);
    mm::HttpServerModule http;
    http.setScheduler(&s);
    using OpResult = mm::HttpServerModule::OpResult;

    REQUIRE(http.applyAddModule("Knob", "A", "Root") == OpResult::Ok);
    REQUIRE(http.applyAddModule("Knob", "B", "Root") == OpResult::Ok);
    CHECK(root->childCount() == 2);

    CHECK(http.applyClearChildren("Root") == OpResult::Ok);
    CHECK(root->childCount() == 0);

    // Clearing a non-existent parent is ModuleNotFound, not a crash. Clearing an
    // already-empty container is Ok.
    CHECK(http.applyClearChildren("Nope") == OpResult::ModuleNotFound);
    CHECK(http.applyClearChildren("Root") == OpResult::Ok);

    s.deleteTree(root);
}

TEST_CASE("apply-core: applyOp dispatches each op type and tolerates bad input") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box();
    root->setName("Root");
    s.addModule(root);
    mm::HttpServerModule http;
    http.setScheduler(&s);
    using OpResult = mm::HttpServerModule::OpResult;

    // The op JSON shapes are exactly what the installer pushes over APPLY_OP.
    CHECK(http.applyOp("{\"op\":\"add\",\"type\":\"Knob\",\"id\":\"K\",\"parent\":\"Root\"}") == OpResult::Ok);
    CHECK(childNamed(root, "K") != nullptr);

    CHECK(http.applyOp("{\"op\":\"set\",\"module\":\"K\",\"control\":\"value\",\"value\":7}") == OpResult::Ok);
    CHECK(static_cast<Knob*>(childNamed(root, "K"))->value == 7);

    CHECK(http.applyOp("{\"op\":\"clearChildren\",\"parent\":\"Root\"}") == OpResult::Ok);
    CHECK(root->childCount() == 0);

    // Unknown op verb and a malformed (no "op") object are BadRequest, not crashes —
    // the robustness rule: any pushed bytes are tolerated.
    CHECK(http.applyOp("{\"op\":\"frobnicate\"}") == OpResult::BadRequest);
    CHECK(http.applyOp("{\"nope\":1}") == OpResult::BadRequest);

    s.deleteTree(root);
}

// A per-control validator (like SystemModule.deviceModel's printable-ASCII rule) is
// enforced THROUGH the apply-core — so the APPLY_OP `set` the installer pushes over
// serial is guarded exactly like an HTTP write, with no per-transport special-casing.
// This is the point of moving validation onto the control: one backend check, every path.
TEST_CASE("apply-core: a control validator rejects bad input on the set/APPLY_OP path") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box();
    root->setName("Root");
    s.addModule(root);
    mm::HttpServerModule http;
    http.setScheduler(&s);
    using OpResult = mm::HttpServerModule::OpResult;

    REQUIRE(http.applyAddModule("Tag", "T", "Root") == OpResult::Ok);
    auto* tag = static_cast<Tag*>(childNamed(root, "T"));
    REQUIRE(tag != nullptr);

    // Valid value applies — via applySetControl (HTTP path) ...
    CHECK(http.applySetControl("T", "label", "{\"value\":\"LOLIN D32\"}") == OpResult::Ok);
    CHECK(std::strcmp(tag->label, "LOLIN D32") == 0);

    // ... and via applyOp (the APPLY_OP-over-serial path) — same shape the installer sends.
    CHECK(http.applyOp("{\"op\":\"set\",\"module\":\"T\",\"control\":\"label\",\"value\":\"Olimex Gateway\"}")
          == OpResult::Ok);
    CHECK(std::strcmp(tag->label, "Olimex Gateway") == 0);

    // A raw control byte in the value → Malformed on the APPLY_OP path, prior value kept.
    const char badOp[] = {'{','"','o','p','"',':','"','s','e','t','"',',',
                          '"','m','o','d','u','l','e','"',':','"','T','"',',',
                          '"','c','o','n','t','r','o','l','"',':','"','l','a','b','e','l','"',',',
                          '"','v','a','l','u','e','"',':','"','x', 0x01, '"','}', 0};
    CHECK(http.applyOp(badOp) == OpResult::Malformed);
    CHECK(std::strcmp(tag->label, "Olimex Gateway") == 0);   // unchanged — no partial write

    // Empty string → Malformed too (the validator rejects 0-length), prior value kept.
    CHECK(http.applySetControl("T", "label", "{\"value\":\"\"}") == OpResult::Malformed);
    CHECK(std::strcmp(tag->label, "Olimex Gateway") == 0);

    s.deleteTree(root);
}
