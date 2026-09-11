// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file MoonStatsModule.h
/// MoonStats: consent, and the once-per-install trigger.
///
/// Owns the two things that decide whether a report is ever built: what the user answered, and
/// whether the firmware version changed since the last time this ran. It does NOT build the report
/// (that is [MoonStatsReport.h](MoonStatsReport.h), a pure function) and it does not send it.
///
/// **Consent is the gate, and declining is silent.** `consent` is Unanswered until the user picks.
/// Never is remembered forever, and a device that never gets a Yes opens no connection and computes
/// no identifier: there is no "declined" record, because sending one would be a report.
///
/// **The trigger is a version comparison, not a timer.** `reportedVersion` persists the version that
/// last produced a report. When the running version differs, one report is due; afterwards the two
/// match and nothing is due until the next upgrade. So a reboot sends nothing, and an upgrade sends
/// exactly one report. Whether that report says `install` or `upgrade` follows from the same field:
/// an empty `reportedVersion` on a device that has never reported is a fresh install, a different
/// one is an upgrade. No identifier is involved in telling those apart.
///
/// Suppressed in AP mode, where there is no route to the internet and the user is usually
/// mid-provisioning: prompting there asks a question about data before the device can even reach
/// the network.
///
/// See [privacy-policy.md](../../docs/privacy-policy.md) for what is promised, and
/// [the MoonCloud plan](../../docs/history/plans/Plan-20260910 - MoonCloud.md) for the id.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/MoonModule.h"
#include "core/MoonCloudModule.h"
#include "core/Control.h"
#include "core/JsonSink.h"
#include "core/Scheduler.h"
#include "core/build_info.h"
#include "platform/platform.h"

namespace mm {

// -----------------------------------------------------------------------------------------------
// The report itself: a pure function over the live module tree.
//
// MoonStats: the one report projectMM sends, and only after the user says yes.
//
// A PURE FUNCTION over the live module tree. It opens no socket, reads no consent, persists
// nothing, and can be called from a test with a tree built by hand. Everything it reports is
// already in memory as a control or a module name, so this is a serializer over known facts
// rather than new instrumentation.
//
// **The allowlist is the design.** Fields are named one at a time and copied by name. A builder
// that walked the tree and emitted what it found would leak on its first run: `deviceName` and
// `mac` are live SystemModule controls sitting beside `chip` and `flash`, and the network SSID and
// credentials are controls too. So there is no "emit everything except" path here, and
// `unit_MoonStatsReport.cpp` asserts the forbidden names cannot appear whatever the tree holds.
// See [privacy-policy.md](../../docs/privacy-policy.md), which is the promise this implements, and
// [the MoonCloud plan](../../docs/history/plans/Plan-20260910 - MoonCloud.md) for the id.

/// What the report says happened. The device knows which without an identifier: the trigger is a
/// stored version file changing, so a report carrying a previous version is an upgrade and one
/// without is a fresh install.
enum class MoonStatsEvent : uint8_t { Install, Upgrade };

/// Serialize the report into `sink`.
///
/// @param sink        where the JSON object lands.
/// @param root        the module tree to read (Scheduler's modules in production, a hand-built
///                    tree in a test).
/// @param moduleCount how many modules `root` holds.
/// @param event       install or upgrade.
/// @param installationId 32 hex characters from `platform::installationId()`, or nullptr to omit
///                    it (which is what a test asserting the forbidden fields passes).
/// @param version     the version now running.
/// @param previousVersion the version it replaced, or nullptr on a fresh install.

/// Read one control's value out of `mod` BY NAME, as a string, or return false when the module
/// does not carry it. Named lookup rather than a cached descriptor: the report is built once per
/// install, so a linear walk of ~20 controls costs nothing, and a control that gets renamed makes
/// the field disappear rather than emitting whatever moved into its index.
inline bool readControl(const MoonModule* mod, const char* name, JsonSink& out) {
    if (!mod) return false;
    auto& ctrls = mod->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        auto& c = ctrls[i];
        if (c.name && std::strcmp(c.name, name) == 0) {
            writeControlValue(out, c);
            return true;
        }
    }
    return false;
}

/// Find a module by name anywhere in the tree, depth first.
inline const MoonModule* findModule(MoonModule* const* root, uint8_t count, const char* name) {
    for (uint8_t i = 0; i < count; i++) {
        const MoonModule* m = root[i];
        if (!m) continue;
        if (m->name() && std::strcmp(m->name(), name) == 0) return m;
        for (uint8_t c = 0; c < m->childCount(); c++) {
            if (const MoonModule* ch = m->child(c)) {
                if (ch->name() && std::strcmp(ch->name(), name) == 0) return ch;
            }
        }
    }
    return nullptr;
}

/// Copy one named control into the report under the same key, or omit the key entirely when the
/// control is absent. Omission rather than a null: an absent field costs no bytes and the server
/// counts what it sees, where a null would need a rule about what it means.
inline void field(JsonSink& sink, const MoonModule* mod, const char* control, const char* key,
           bool& first) {
    if (!mod) return;
    JsonSink value;
    if (!readControl(mod, control, value)) return;
    if (!first) sink.append(",");
    first = false;
    sink.append("\"");
    sink.append(key);
    sink.append("\":");
    sink.append(value.data());
}


inline void buildMoonStatsReport(JsonSink& sink,
                          MoonModule* const* root, uint8_t moduleCount,
                          MoonStatsEvent event,
                          const char* installationId,
                          const char* version,
                          const char* previousVersion) {
    const MoonModule* system = findModule(root, moduleCount, "System");

    sink.append("{");
    bool first = true;

    // The installation id, when the caller has one. Omitted entirely by the test that asserts the
    // forbidden fields, and by any caller that has not obtained consent.
    if (installationId && *installationId) {
        sink.append("\"installationId\":");
        sink.writeJsonString(installationId);
        first = false;
    }

    if (!first) sink.append(",");
    sink.append("\"event\":");
    sink.writeJsonString(event == MoonStatsEvent::Upgrade ? "upgrade" : "install");
    first = false;

    if (version && *version) {
        sink.append(",\"version\":");
        sink.writeJsonString(version);
    }

    // Whether this firmware came from a release or somebody's laptop. `kRelease` is set by CI on a
    // published build and EMPTY on a local one, so this needs no new plumbing and no question to the
    // user: it describes the binary, not the person.
    //
    // Sent from the FIRST report rather than added when the noise becomes annoying, because a flag
    // introduced later cannot classify rows already stored: every report before it exists is
    // permanently unlabeled, and nothing afterwards can tell a developer's bench board from a
    // user's shelf. One boolean now buys a dashboard that can say "excluding development installs"
    // for its whole history.
    sink.append(",\"dev\":");
    sink.writeBool(kRelease[0] == 0);
    // Present only on an upgrade, and it is what tells the server this was one.
    if (previousVersion && *previousVersion) {
        sink.append(",\"previousVersion\":");
        sink.writeJsonString(previousVersion);
    }

    // Hardware, straight off the System module. Every one of these is a fact about the board, not
    // about the person holding it. `deviceName` and `mac` sit in the same control list and are
    // deliberately NOT here.
    field(sink, system, "chip", "chip", first);
    field(sink, system, "flash", "flash", first);
    field(sink, system, "psramType", "psram", first);
    field(sink, system, "sdk", "sdk", first);
    field(sink, system, "deviceModel", "deviceModel", first);

    // Which modules are enabled, by name. The names are ours (a fixed vocabulary from the
    // catalog), not anything a user typed.
    sink.append(",\"modules\":[");
    bool firstModule = true;
    for (uint8_t i = 0; i < moduleCount; i++) {
        const MoonModule* m = root[i];
        if (!m || !m->enabled() || !m->name()) continue;
        if (!firstModule) sink.append(",");
        firstModule = false;
        sink.writeJsonString(m->name());
    }
    sink.append("]");

    sink.append("}");
    sink.flush();
}


class MoonStatsModule : public MoonModule {
public:
    /// What the user answered. Persisted, so Never survives a reboot and an upgrade.
    ///
    /// Unanswered is the default and the only state that shows a prompt. NotNow exists so a user
    /// who is busy is asked again after the NEXT upgrade rather than never: it is a deferral, where
    /// Never is an answer.
    enum Consent : uint8_t { Unanswered = 0, Yes = 1, NotNow = 2, Never = 3 };

    void setup() override {
        std::snprintf(runningVersion_, sizeof(runningVersion_), "%s", kVersion);
        MoonModule::setup();
    }

    void defineControls() override {
        controls_.clear();
        // The user's answer. A select rather than a bool: "not now" and "never" are different
        // answers, and collapsing them would either nag someone who declined or silence someone
        // who only deferred.
        static const char* kConsentOptions[] = {"Not answered", "Yes", "Not now", "Never"};
        controls_.addSelect("consent", consent_, kConsentOptions, 4);

        // The version that last produced a report. Empty means none ever has, which is what makes
        // the first report an `install`. Read-only in the UI: it is bookkeeping, and a user editing
        // it would either re-send or silence a genuine upgrade.
        controls_.addReadOnly("reportedVersion", reportedVersion_, sizeof(reportedVersion_));
        controls_.addReadOnly("version", runningVersion_, sizeof(runningVersion_));

    }

    /// True when the UI should ask. Only ever true before the user answers, and never while the
    /// device is its own access point.
    bool shouldPrompt() const {
        if (consent_ != Unanswered) return false;
        // Not while the device is its own access point: there is no route to the internet there,
        // and the user is part-way through setting the device up. The question waits for a real
        // network rather than interrupting provisioning.
        if (inApMode()) return false;
        return true;
    }

    /// True when a report is due: the user said yes, and the running version differs from the one
    /// that last reported.
    bool reportDue() const {
        if (consent_ != Yes) return false;
        return std::strcmp(reportedVersion_, runningVersion_) != 0;
    }

    /// Which kind of report is due. An empty `reportedVersion` means this install has never
    /// reported, so it is a fresh install; anything else means the firmware moved under it.
    MoonStatsEvent dueEvent() const {
        return reportedVersion_[0] == 0 ? MoonStatsEvent::Install : MoonStatsEvent::Upgrade;
    }

    /// The version being replaced, for an upgrade report, or nullptr on a fresh install.
    const char* previousVersion() const {
        return reportedVersion_[0] == 0 ? nullptr : reportedVersion_;
    }

    /// Record that a report was sent for the running version, so nothing further is due until the
    /// next upgrade. Called after a send is HANDED OFF rather than after it succeeds: a failure is
    /// not retried (a lost report costs one row in an aggregate), and marking only on success would
    /// make an unreachable server re-send on every boot.
    void markReported() {
        std::snprintf(reportedVersion_, sizeof(reportedVersion_), "%s", runningVersion_);
        markDirty();
    }

    /// Record the user's answer.
    void setConsent(Consent answer) {
        consent_ = static_cast<uint8_t>(answer);
        markDirty();
    }

    Consent consent() const { return static_cast<Consent>(consent_); }

    /// Pretend the firmware is a different version, so a test can exercise a real UPGRADE: the
    /// running version changing under an install that already reported. Nothing else can produce
    /// that state, since `setup()` reads a compile-time constant and a test cannot recompile.
    void setRunningVersionForTest(const char* v) {
        std::snprintf(runningVersion_, sizeof(runningVersion_), "%s", v ? v : "");
    }

    /// This installation's id, or an empty string when the user has not consented.
    ///
    /// Gated on consent rather than merely unused without it: the policy says nothing is generated
    /// until you say yes, so a caller cannot obtain one to log or display either.
    void installationId(char* out) const {
        if (!out) return;
        if (consent_ != Yes) { out[0] = 0; return; }
        mm::installationId(out);
    }

    /// One bounded HTTP call per second at most, and only when a report is actually due, which is
    /// once per install or upgrade in the life of a device. The same shape HueDriver uses for the
    /// bridge poll: `tick1s` is not the render path, and a call that costs a second here costs
    /// nothing a user can see.
    // The send is a bounded blocking call, which is what `tick1s` is for: it is the 1 Hz
    // housekeeping tick, not the per-frame render path, and it is where HueDriver polls its bridge
    // and the OTA path fetches. `-Wfunction-effects` still warns, because the base declares the
    // hook MM_NONBLOCKING for the per-frame case and an override cannot narrow that: the warning
    // names a real property (this call blocks) rather than a mistake.
    //
    // What keeps it acceptable is frequency: at most one call, once per firmware install, for the
    // life of the device. The guards below run first and cost nothing, so a device that has already
    // reported never reaches the blocking part again.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        if (!reportDue()) return;
        if (!platform::networkReady()) return;   // nothing to do yet; try again next second
        // Serving our own AP means no route out, so a send would fail and mark itself reported.
        if (inApMode()) return;
        sendReport();
    }

    /// Whether the device is currently serving its own access point, asked of the platform rather
    /// than pushed in by NetworkModule. A setter would have meant one module reaching into another
    /// to keep a copy of a fact the platform already answers, and a stale copy is a prompt that
    /// appears at the wrong moment.
    bool inApMode() const { return platform::wifiApConnected(); }

private:
    /// Build the report and POST it once. Marked reported on HAND-OFF rather than on success: a
    /// failure costs one row in an aggregate, where re-sending on every boot until a server answers
    /// would turn one report into a heartbeat, which is the one thing this feature promises never
    /// to be.
    void sendReport() {
        char id[kInstallationIdChars + 1] = {};
        installationId(id);
        if (!id[0]) return;   // no consent, no id, no report

        // Scheduler exposes module(i) rather than the array, so the tree is gathered here. 32 is
        // its own capacity, so this cannot truncate a tree the scheduler accepted.
        MoonModule* tree[32] = {};
        auto* sched = Scheduler::instance();
        if (!sched) return;
        uint8_t count = 0;
        for (uint8_t i = 0; i < sched->moduleCount() && count < 32; i++) {
            if (MoonModule* m = sched->module(i)) tree[count++] = m;
        }

        JsonSink body;
        buildMoonStatsReport(body, tree, count,
                             dueEvent(), id, runningVersion_, previousVersion());

        // The response is discarded: the server answers {"ok":true} and there is nothing to do
        // with it. A small buffer still has to exist because httpRequest reads into one.
        // Sent through the container, which owns the address and the scheme choice.
        if (auto* cloud = static_cast<const MoonCloudModule*>(parent())) {
            (void)cloud->post("/api/report", body.data());
        }
        markReported();
    }

    uint8_t consent_ = Unanswered;
    char reportedVersion_[32] = {};
    char runningVersion_[32] = {};
};

}  // namespace mm
