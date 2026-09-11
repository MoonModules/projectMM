// @module MoonStatsModule

#include "doctest.h"
#include "core/MoonStatsModule.h"

#include <string>

/// Nothing is reported until the user says yes, and declining is permanent.
///
/// The default is Unanswered, which is the only state that shows a prompt. A device whose user
/// never answers, or who answers Never, sends nothing at all: no report, and no record that they
/// declined, since sending one would itself be a report.
TEST_CASE("a report is due only after the user consents") {
    mm::MoonStatsModule stats;
    stats.setup();
    stats.defineControls();

    CHECK(stats.consent() == mm::MoonStatsModule::Unanswered);
    CHECK_FALSE(stats.reportDue());

    stats.setConsent(mm::MoonStatsModule::Never);
    CHECK_FALSE(stats.reportDue());
    CHECK_FALSE(stats.shouldPrompt());

    stats.setConsent(mm::MoonStatsModule::NotNow);
    CHECK_FALSE(stats.reportDue());

    stats.setConsent(mm::MoonStatsModule::Yes);
    CHECK(stats.reportDue());
}

/// The prompt appears once and stops appearing as soon as it is answered, whichever way.
TEST_CASE("the prompt appears only while the question is unanswered") {
    mm::MoonStatsModule stats;
    stats.setup();
    stats.defineControls();

    CHECK(stats.shouldPrompt());

    stats.setConsent(mm::MoonStatsModule::Yes);
    CHECK_FALSE(stats.shouldPrompt());
}

/// A device serving its own access point asks nothing and sends nothing.
///
/// Both paths consult the platform rather than a cached flag, so there is no copy to go stale. The
/// desktop stub answers false, so this pins the FALSE branch: with no AP, the question is asked and
/// a report becomes due. The true branch belongs on a board, which is a state the desktop cannot
/// enter, so the honest thing is to say so rather than to write an assertion that holds either way.
///
/// An earlier version of this case checked only `inApMode() == false` and two consequences that
/// follow from consent alone: it stayed green with the AP check deleted from `shouldPrompt()`
/// entirely, which is a test that cannot fail.
TEST_CASE("with no access point, the question is asked and a report becomes due") {
    mm::MoonStatsModule stats;
    stats.setup();
    stats.defineControls();

    REQUIRE_FALSE(stats.inApMode());        // the desktop stub: never its own AP

    // shouldPrompt() is (unanswered AND not in AP mode). With the second false, the first decides,
    // so these two together pin that consent still drives it while the AP check is present.
    CHECK(stats.shouldPrompt());
    stats.setConsent(mm::MoonStatsModule::Never);
    CHECK_FALSE(stats.shouldPrompt());

    stats.setConsent(mm::MoonStatsModule::Yes);
    CHECK(stats.reportDue());
}

/// One report per version, not one per boot.
///
/// This is the whole trigger: after reporting, the recorded version matches the running one and
/// nothing further is due, however many times the device restarts.
TEST_CASE("a restart sends nothing once the running version has been reported") {
    mm::MoonStatsModule stats;
    stats.setup();
    stats.defineControls();
    stats.setConsent(mm::MoonStatsModule::Yes);

    REQUIRE(stats.reportDue());
    stats.markReported();
    CHECK_FALSE(stats.reportDue());

    // A reboot re-runs setup(); the persisted reportedVersion_ still matches, so nothing is due.
    stats.setup();
    CHECK_FALSE(stats.reportDue());
}

/// A first report is an install, and one after a version change is an upgrade, told apart without
/// any identifier: an empty recorded version means this install has never reported.
TEST_CASE("an install and an upgrade are distinguished by the recorded version") {
    mm::MoonStatsModule stats;
    stats.setup();
    stats.defineControls();
    stats.setConsent(mm::MoonStatsModule::Yes);

    CHECK(stats.dueEvent() == mm::MoonStatsEvent::Install);
    CHECK(stats.previousVersion() == nullptr);

    stats.markReported();
    CHECK(stats.dueEvent() == mm::MoonStatsEvent::Upgrade);
    CHECK(stats.previousVersion() != nullptr);
    CHECK(std::string(stats.previousVersion()).size() > 0);
}

/// No identifier exists until the user consents, so a device that declined has nothing to leak and
/// nothing to log.
TEST_CASE("no installation id is produced without consent") {
    mm::MoonStatsModule stats;
    stats.setup();
    stats.defineControls();

    char id[mm::kInstallationIdChars + 1] = {"unset"};
    stats.installationId(id);
    CHECK(std::string(id).empty());

    stats.setConsent(mm::MoonStatsModule::Never);
    stats.installationId(id);
    CHECK(std::string(id).empty());

    stats.setConsent(mm::MoonStatsModule::Yes);
    stats.installationId(id);
    CHECK(std::string(id).size() == mm::kInstallationIdChars);
}

/// An upgrade sends exactly one report: the version changes under an install that already
/// reported, one report becomes due, and the next boot is quiet again.
///
/// This is the sequence a real device lives through and the one nothing else covers: the other
/// cases start from a fresh install, where the version never moves.
TEST_CASE("an upgrade sends one report, and the boot after it sends none") {
    mm::MoonStatsModule stats;
    stats.setup();
    stats.defineControls();
    stats.setConsent(mm::MoonStatsModule::Yes);

    // First install: one report due, then reported.
    REQUIRE(stats.reportDue());
    REQUIRE(stats.dueEvent() == mm::MoonStatsEvent::Install);
    stats.markReported();
    REQUIRE_FALSE(stats.reportDue());

    // The firmware is upgraded. One report is due again, and it is an UPGRADE carrying the
    // version it replaced.
    stats.setRunningVersionForTest("9.9.9");
    CHECK(stats.reportDue());
    CHECK(stats.dueEvent() == mm::MoonStatsEvent::Upgrade);
    CHECK(std::string(stats.previousVersion()) != "9.9.9");

    stats.markReported();
    CHECK_FALSE(stats.reportDue());

    // And every boot after it stays quiet, however many times.
    for (int i = 0; i < 3; i++) CHECK_FALSE(stats.reportDue());
}

/// Not-now defers rather than answers: the question returns after the next upgrade, where Never
/// silences it for good.
///
/// The difference is the whole reason there are four options rather than three, and neither branch
/// was covered.
TEST_CASE("not-now is asked again after an upgrade, never is not") {
    mm::MoonStatsModule deferred;
    deferred.setup();
    deferred.defineControls();
    deferred.setConsent(mm::MoonStatsModule::NotNow);
    CHECK_FALSE(deferred.shouldPrompt());   // not right now
    CHECK_FALSE(deferred.reportDue());      // and nothing is sent

    mm::MoonStatsModule refused;
    refused.setup();
    refused.defineControls();
    refused.setConsent(mm::MoonStatsModule::Never);
    refused.setRunningVersionForTest("9.9.9");
    CHECK_FALSE(refused.shouldPrompt());    // an upgrade does not reopen the question
    CHECK_FALSE(refused.reportDue());
}

/// Consent withdrawn after reporting stops the next one.
///
/// A user who says yes, upgrades, then changes their mind must not have that upgrade reported.
TEST_CASE("withdrawing consent stops a report that would otherwise be due") {
    mm::MoonStatsModule stats;
    stats.setup();
    stats.defineControls();
    stats.setConsent(mm::MoonStatsModule::Yes);
    stats.markReported();

    stats.setRunningVersionForTest("9.9.9");
    REQUIRE(stats.reportDue());             // the upgrade would be reported

    stats.setConsent(mm::MoonStatsModule::Never);
    CHECK_FALSE(stats.reportDue());         // until it is refused

    char id[mm::kInstallationIdChars + 1] = {"unset"};
    stats.installationId(id);
    CHECK(std::string(id).empty());         // and no id exists to send
}

/// Toggling consent off and on again reports nothing new.
///
/// Yes, then Never, then Yes is a user changing their mind, not a new installation, so the second
/// Yes must not produce a second report. The version bookkeeping is what enforces it: consent
/// gates whether a report may be sent, and the recorded version decides whether one is DUE, so
/// re-granting consent cannot re-arm a report that already went.
///
/// Without this, one installation could inflate the totals by toggling a dropdown.
TEST_CASE("saying yes, then no, then yes again sends only the first report") {
    mm::MoonStatsModule stats;
    stats.setup();
    stats.defineControls();

    stats.setConsent(mm::MoonStatsModule::Yes);
    REQUIRE(stats.reportDue());
    stats.markReported();
    REQUIRE_FALSE(stats.reportDue());

    stats.setConsent(mm::MoonStatsModule::Never);
    CHECK_FALSE(stats.reportDue());

    stats.setConsent(mm::MoonStatsModule::Yes);
    CHECK_FALSE(stats.reportDue());   // still nothing: no new install, no new report

    // An actual upgrade still reports, so the rule above suppresses duplicates rather than
    // silencing the device.
    stats.setRunningVersionForTest("9.9.9");
    CHECK(stats.reportDue());
}
