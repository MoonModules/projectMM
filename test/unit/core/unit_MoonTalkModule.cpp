// @module MoonTalkModule

#include "doctest.h"
#include "core/MoonTalkModule.h"

#include <string>

/// Naming is a SECOND consent on top of posting, and it starts off.
///
/// A device name identifies a person where an installation id does not, so agreeing to publish
/// messages is not agreeing to be named. `sharesName()` requires BOTH, which is what keeps the two
/// decisions separate.
TEST_CASE("naming is a second consent, off by default") {
    mm::MoonTalkModule talk;
    talk.defineControls();

    CHECK(talk.consent() == mm::MoonTalkModule::Unanswered);
    CHECK_FALSE(talk.sharesName());
}

/// The device name is READ from the module tree, never held as a copy.
///
/// An earlier shape had a `setDeviceName()` setter that nothing ever called, so the name stayed
/// empty and every message went out anonymous while the `shareName` toggle said otherwise: the UI
/// promised something the wire did not deliver, and only reading the stored rows revealed it.
///
/// With no System module present the lookup yields an empty string rather than misbehaving, which
/// is the case a probe instance (built by /api/types and thrown away) actually hits.
TEST_CASE("the device name is looked up rather than stored") {
    mm::MoonTalkModule talk;
    talk.defineControls();

    const char* name = talk.deviceName();
    REQUIRE(name != nullptr);            // never null, whatever the tree holds
    CHECK(std::string(name).size() < 32);
}
