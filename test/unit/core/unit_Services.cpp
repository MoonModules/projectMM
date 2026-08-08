// @module Services

// Pins the Services container's one job: it accepts user-added `service`-role children
// (the core-domain twin of Effects/Drivers), while System — now fixed infrastructure —
// accepts none. Together these two checks are the device-side half of the System/Services
// split: the UI's add/delete affordance follows acceptsChildRoles, so "Services is where you
// add Audio/IR, System is fixed" reduces to these two strings.

#include "doctest.h"
#include "core/Services.h"
#include "core/SystemModule.h"
#include "core/MoonModule.h"

#include <cstring>

using namespace mm;

namespace {
// A stand-in Service-role child (what Audio/IR are): the container accepts it by role.
struct FakeService : MoonModule {
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }
};
} // namespace

TEST_CASE("Services accepts service-role children; System accepts none") {
    Services services;
    CHECK(std::strcmp(services.acceptsChildRoles(), "service") == 0);

    SystemModule sys;
    CHECK(std::strcmp(sys.acceptsChildRoles(), "") == 0);
}

TEST_CASE("Services is a thin grouping node — a service child attaches and ticks") {
    Services services;
    FakeService audio;
    services.addChild(&audio);

    REQUIRE(services.childCount() == 1);
    CHECK(services.child(0) == &audio);
    CHECK(services.child(0)->role() == ModuleRole::Service);

    // The container has no controls of its own (like Effects) — it's pure structure.
    services.defineControls();
    CHECK(services.controls().count() == 0);
}
