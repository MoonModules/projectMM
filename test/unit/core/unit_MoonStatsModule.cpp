// @module MoonStatsModule

#include "doctest.h"
#include "core/FilesystemModule.h"
#include "core/MoonStatsModule.h"
#include "core/Scheduler.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

    CHECK_FALSE(stats.consent());
    CHECK_FALSE(stats.reportDue());

    stats.setConsent(false);
    CHECK_FALSE(stats.reportDue());

    stats.setConsent(false);
    CHECK_FALSE(stats.reportDue());

    stats.setConsent(true);
    CHECK(stats.reportDue());
}

/// The prompt appears once and stops as soon as the question is answered, whichever way.
///
/// The UI asks on `consent == Unanswered` and nothing else (app.js), so that state IS the prompt:
/// an earlier helper duplicated that rule in C++ where nothing called it.
TEST_CASE("the prompt appears only while the question is unanswered") {
    mm::MoonStatsModule stats;
    stats.setup();
    stats.defineControls();

    CHECK_FALSE(stats.consent());

    stats.setConsent(true);
    CHECK(stats.consent());
}

/// A device serving its own access point sends nothing.
///
/// `inApMode()` consults the platform rather than a cached flag, so there is no copy to go stale.
/// The desktop stub answers false, so this pins the FALSE branch: with no AP, a consented device
/// has a report due. The true branch belongs on a board, a state the desktop cannot enter, so the
/// honest thing is to say so rather than write an assertion that holds either way.
TEST_CASE("with no access point, a consented device has a report due") {
    mm::MoonStatsModule stats;
    stats.setup();
    stats.defineControls();

    REQUIRE_FALSE(stats.inApMode());        // the desktop stub: never its own AP

    stats.setConsent(true);
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
    stats.setConsent(true);

    REQUIRE(stats.reportDue());
    stats.markReported();
    CHECK_FALSE(stats.reportDue());

    // A reboot is a NEW instance reading persisted state. The value travels through the CONTROL,
    // which is the mechanism that was broken: `addReadOnly` registers a type the persistence layer
    // skips, so reportedVersion came back empty on every boot and a consented device re-reported
    // forever. Writing it the way a config load does is what proves the fix.
    const mm::ControlList& ctrls = stats.controls();
    uint8_t idx = ctrls.count();
    for (uint8_t i = 0; i < ctrls.count(); i++)
        if (ctrls[i].name && std::strcmp(ctrls[i].name, "reportedVersion") == 0) idx = i;
    REQUIRE(idx < ctrls.count());
    // THE assertion: a control the persistence layer skips saves nothing, and `addReadOnly`
    // registered exactly such a type. Without this the case below passes on a value that never
    // travelled.
    REQUIRE(mm::isPersistable(ctrls[idx]));

    // Restore it the way a config load does: parse the value out of a JSON object by key, with the
    // tolerant policy persistence uses.
    char saved[128];
    std::snprintf(saved, sizeof(saved), "{\"reportedVersion\":\"%s\"}",
                  static_cast<const char*>(ctrls[idx].ptr));

    mm::MoonStatsModule rebooted;
    rebooted.defineControls();
    REQUIRE(mm::applyControlValue(rebooted.controls()[idx], saved, "reportedVersion",
                                  mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    rebooted.setup();
    rebooted.setConsent(true);
    CHECK_FALSE(rebooted.reportDue());
}


/// A first report is an install, and one after a version change is an upgrade, told apart without
/// any identifier: an empty recorded version means this install has never reported.
TEST_CASE("an install and an upgrade are distinguished by the recorded version") {
    mm::MoonStatsModule stats;
    stats.setup();
    stats.defineControls();
    stats.setConsent(true);

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

    stats.setConsent(false);
    stats.installationId(id);
    CHECK(std::string(id).empty());

    stats.setConsent(true);
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
    stats.setConsent(true);

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

/// Declining stays declined, including across an upgrade.
///
/// The four-option consent (unanswered / yes / not now / never) collapsed to a checkbox: both ways
/// of saying no meant nothing is sent, and telling them apart cost a persisted version and a branch
/// in setup(). What remains is the contract that matters: off sends nothing, and a firmware change
/// does not quietly turn it back on.
TEST_CASE("declining survives a reboot and an upgrade") {
    mm::MoonStatsModule declined;
    declined.setup();
    declined.defineControls();
    declined.setConsent(false);
    CHECK_FALSE(declined.reportDue());

    declined.setup();                       // a reboot
    CHECK_FALSE(declined.consent());
    CHECK_FALSE(declined.reportDue());

    declined.setRunningVersionForTest("9.9.9");   // and an upgrade under it
    declined.setup();
    CHECK_FALSE(declined.consent());
    CHECK_FALSE(declined.reportDue());
}

/// Consent withdrawn after reporting stops the next one.
///
/// A user who says yes, upgrades, then changes their mind must not have that upgrade reported.
TEST_CASE("withdrawing consent stops a report that would otherwise be due") {
    mm::MoonStatsModule stats;
    stats.setup();
    stats.defineControls();
    stats.setConsent(true);
    stats.markReported();

    stats.setRunningVersionForTest("9.9.9");
    REQUIRE(stats.reportDue());             // the upgrade would be reported

    stats.setConsent(false);
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

    stats.setConsent(true);
    REQUIRE(stats.reportDue());
    stats.markReported();
    REQUIRE_FALSE(stats.reportDue());

    stats.setConsent(false);
    CHECK_FALSE(stats.reportDue());

    stats.setConsent(true);
    CHECK_FALSE(stats.reportDue());   // still nothing: no new install, no new report

    // An actual upgrade still reports, so the rule above suppresses duplicates rather than
    // silencing the device.
    stats.setRunningVersionForTest("9.9.9");
    CHECK(stats.reportDue());
}


/// Reporting SCHEDULES the save, it does not merely mark the module dirty.
///
/// `markDirty()` flags the module; only `FilesystemModule::noteDirty()` sets the pending flag that
/// `FilesystemModule::tick1s` needs before it flushes. Nothing on the report path called it, and the
/// gap hid behind ordinary use: on a first INSTALL the consent write leaves a 2 s debounce pending,
/// so `reportedVersion` rode along with that flush and looked saved. On an UPGRADE boot consent is
/// already on and no control is written, so the new version stayed in RAM and the next reboot
/// reported the same upgrade again, which is the one case the feature exists for.
///
/// Pinned through the FILE, because the flag is private and the file is what a reboot reads.
TEST_CASE("a report schedules the save that records it") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_stats_save_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(tmpRoot);

    {
        // Scheduler::release() deletes its tree, so the modules are heap-allocated.
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        auto* stats = new mm::MoonStatsModule();
        fs->setTypeName("FilesystemModule");
        stats->setTypeName("MoonStatsModule");
        stats->setName("Stats");
        fs->setScheduler(&scheduler);
        scheduler.addModule(fs);
        scheduler.addModule(stats);
        scheduler.setup();

        stats->setConsent(true);
        REQUIRE(stats->reportDue());

        // What sendReport() does once the POST is handed off. No control is written here, which is
        // exactly the upgrade-boot shape.
        stats->markReported();
        CHECK_FALSE(stats->reportDue());

        // Past the debounce, so the pending save lands. Without noteDirty() the flag is never set
        // and this tick returns before flushing.
        mm::platform::setTestNowMs(mm::platform::millis() + 5000);
        fs->tick1s();
        mm::platform::setTestNowMs(0);

        char path[512];
        std::snprintf(path, sizeof(path), "%s/.config/MoonStatsModule.json", tmpRoot);
        REQUIRE(std::filesystem::exists(path));

        std::ifstream in(path);
        const std::string saved((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
        // The VERSION, not merely the key: an empty value is what the unsaved shape looked like.
        char expected[64];
        std::snprintf(expected, sizeof(expected), "\"reportedVersion\":\"%s\"", mm::kVersion);
        CHECK(saved.find(expected) != std::string::npos);

        scheduler.release();
    }

    mm::platform::fsSetRoot("");
    std::filesystem::remove_all(tmpRoot);
}
