// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file MoonStatsModule.h
/// MoonStats: consent, and the once-per-install trigger.
///
/// Owns what decides whether a report is built. It does not build one (that is
/// `buildMoonStatsReport`, a pure function) and does not send it.
///
/// **Consent is the gate, and declining is silent.** A device that never gets a Yes opens no
/// connection and computes no identifier; there is no "declined" record, because sending one would
/// be a report.
///
/// **The trigger is a version comparison, not a timer.** `reportedVersion` persists the version that
/// last reported, so a reboot sends nothing and an upgrade sends exactly one. An empty value means a
/// fresh install, a different one an upgrade: no identifier is involved in telling those apart.
///
/// Suppressed in AP mode, where there is no route out and the user is mid-provisioning.
///
/// See [privacy-policy.md](../../docs/legal/privacy-policy.md) for what is promised.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/MoonModule.h"
#include "core/MoonCloudModule.h"
#include "core/Control.h"
#include "core/FilesystemModule.h"   // noteDirty(): scheduling the save, not just marking it
#include "core/JsonSink.h"
#include "core/Scheduler.h"
#include "core/build_info.h"
#include "platform/platform.h"
#include "core/LightSummary.h"          // lightCount: the POD the light domain publishes
#include "light/drivers/Drivers.h"      // Drivers::latestSummary(): the real light total
#include "core/ModuleFactory.h"      // displayNameFor: the TYPE label, not the instance name
#include "light/moonlive/MoonLiveScriptFile.h"  // isFactoryScript: a shipped name is ours, not yours

namespace mm {

// The report: a PURE FUNCTION over the live module tree. It opens no socket, reads no consent and
// persists nothing, so a test can call it with a tree built by hand.
//
// **The allowlist is the design.** Fields are named one at a time and copied by name. A builder that
// walked the tree and emitted what it found would leak on its first run: `deviceName`, `mac` and the
// network credentials are live controls sitting beside `chip` and `flash`. `unit_MoonStatsReport.cpp`
// asserts the forbidden names cannot appear whatever the tree holds.

/// What the report says happened.
/// Install and Upgrade are automatic and happen once each. Refresh is the user pressing the
/// button: same payload, same consent, sent again because their setup changed in a way no version
/// bump describes (they wired the real fixtures up after reporting a bare board).
enum class MoonStatsEvent : uint8_t { Install, Upgrade, Refresh };

/// Read one control's value out of `mod` by name. Named lookup rather than an index: a renamed
/// control makes the field disappear rather than emitting whatever moved into its slot.
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

/// Find a module by name among the roots and their direct children, which is as deep as the one
/// caller needs (the top-level `System`).
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

/// Copy one named control into the report, or omit the key when the control is absent. Omission
/// rather than a null, which would need a rule about what it means.
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


/// Append every user-added enabled module, depth first, each as `role:name`.
///
/// The role prefix is what lets the server aggregate drivers, layouts, effects and services
/// separately: one list in one column, four charts out of it. `ModuleRole` already exists on every
/// module, so nothing new is invented and nothing a user typed is sent: both halves come from the
/// catalog's fixed vocabulary.
/// The shipped script a module runs, or nullptr when it runs none, runs one a user wrote, or is not
/// a scripted module at all. Only a name from the catalog is returned, so nothing a user typed can
/// reach a caller (the report test pins that).
inline const char* factoryScriptOf(const MoonModule* m) {
    if (!m) return nullptr;
    const ControlList& cs = m->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (cs[i].type != ControlType::FilePath || !cs[i].name) continue;
        if (std::strcmp(cs[i].name, "script") != 0) continue;
        const char* value = static_cast<const char*>(cs[i].ptr);
        return moonlive::isFactoryScript(value) ? value : nullptr;
    }
    return nullptr;
}

inline void reportModules(JsonSink& sink, const MoonModule* const* mods, uint8_t count,
                          bool& first) {
    for (uint8_t i = 0; i < count; i++) {
        const MoonModule* m = mods[i];
        if (!m || !m->enabled()) continue;
        // ROLE decides, not `isWiredByCode()`. Only children are marked wired (main.cpp marks Tasks,
        // Pins, Stats, Talk, Preview, ...); the twelve top-level modules are added with
        // `scheduler.addModule()` and carry no marker, so a wired-by-code test reported every one of
        // them as `generic:System`, `generic:Network` and so on.
        //
        // `Generic` means structural container, which is exactly what should not be counted, and
        // `Layer` is structural too (it holds effects rather than being one a user picks). What
        // remains is what somebody chose: drivers, services, layouts, effects, modifiers.
        const ModuleRole role = m->role();
        // PreviewDriver is boot wiring, not a choice: main.cpp adds it to every device, so it
        // reported once per installation and topped the driver pie with a number that was really
        // the installation count. Excluded BY TYPE rather than by isWiredByCode(), because that
        // flag marks only children: the twelve top-level modules carry no marker, and filtering on
        // it once reported every one of them as `generic:System`. The test below pins that.
        //
        // The other boot-wired modules need no entry here: LightPresets and Devices are Generic,
        // which the role rule already drops. AudioService is deliberately NOT auto-wired (main.cpp:
        // "a mic peripheral, useful only on a board"), so it stays a real user choice and counts.
        const bool prewired = std::strcmp(m->typeName(), "PreviewDriver") == 0;
        if (m->name() && !prewired && role != ModuleRole::Generic && role != ModuleRole::Layer) {
            // A scripted module is "MoonLive" whatever it runs, so the type name alone says nothing
            // about what the device is actually doing. The script name is the interesting half, and
            // it is reported ONLY when it is one we ship: those come from our own catalog, the same
            // fixed vocabulary as a module type. A name the user invented is text they typed, which
            // the report never carries, so it degrades to the bare type name.
            const char* script = factoryScriptOf(m);
            // The TYPE, not the instance name. `name()` is user-editable and carries the
            // uniquifying suffix Scheduler adds for a second instance, so three rings reported
            // `Ring`, `Ring-2`, `Ring-3` as three different layouts, and a renamed module reported
            // whatever its owner typed. displayNameFor turns the factory key into the same label
            // the UI shows (RingLayout -> Ring), which is a fixed vocabulary from our own registry.
            //
            // Falls back to name() when typeName is empty: only ModuleFactory sets it, so a module
            // built any other way would otherwise report an empty string. The returned pointer is a
            // shared static buffer, valid until the next call, which the snprintf below consumes.
            const char* label = m->typeName() && m->typeName()[0]
                                    ? ModuleFactory::displayNameFor(m->typeName(), role)
                                    : m->name();
            char entry[80];
            if (script)
                std::snprintf(entry, sizeof(entry), "%s:%s/%s", roleName(role), label, script);
            else
                std::snprintf(entry, sizeof(entry), "%s:%s", roleName(role), label);
            if (!first) sink.append(",");
            first = false;
            sink.writeJsonString(entry);
        }
        for (uint8_t c = 0; c < m->childCount(); c++) {
            const MoonModule* child = m->child(c);
            reportModules(sink, &child, 1, first);
        }
    }
}

inline void buildMoonStatsReport(JsonSink& sink,
                          MoonModule* const* root, uint8_t moduleCount,
                          MoonStatsEvent event,
                          const char* installationId,
                          const char* version,
                          const char* previousVersion,
                          uint32_t lightCount = 0,
                          uint32_t totalHeap = 0, uint32_t freeHeap = 0,
                          uint32_t fps = 0) {
    const MoonModule* system = findModule(root, moduleCount, "System");

    sink.append("{");
    bool first = true;

    // Omitted by any caller without consent, and by the test asserting the forbidden fields.
    if (installationId && *installationId) {
        sink.append("\"installationId\":");
        sink.writeJsonString(installationId);
        first = false;
    }

    if (!first) sink.append(",");
    sink.append("\"event\":");
    sink.writeJsonString(event == MoonStatsEvent::Refresh ? "refresh"
                         : event == MoonStatsEvent::Upgrade ? "upgrade" : "install");
    first = false;

    if (version && *version) {
        sink.append(",\"version\":");
        sink.writeJsonString(version);
    }

    // Release or somebody's laptop: `kRelease` is set by CI and empty on a local build, so it
    // describes the binary, not the person. Sent from the FIRST report because a flag added later
    // cannot classify rows already stored.
    sink.append(",\"dev\":");
    sink.writeBool(kRelease[0] == 0);
    // Present only on an upgrade, and it is what tells the server this was one.
    if (previousVersion && *previousVersion) {
        sink.append(",\"previousVersion\":");
        sink.writeJsonString(previousVersion);
    }

    // Memory and light count as RAW numbers, bucketed into ranges by the server. Bucketing here
    // would freeze every stored row at today's boundaries: a range that turns out wrong could never
    // be re-cut, which is the same trap as a field that was never collected.
    // Passed in, not read here: the builder is a pure function over its arguments everywhere else,
    // and reading the platform mid-serialize would make these two fields untestable.
    //
    // `fps` is the SYSTEM render rate (Scheduler::fps), not a per-effect number: it says what the
    // whole tree achieves on that hardware, which is the figure a reader compares against their own.
    sink.appendf(",\"totalHeap\":%u,\"freeHeap\":%u,\"lightCount\":%u,\"fps\":%u",
                 static_cast<unsigned>(totalHeap),
                 static_cast<unsigned>(freeHeap),
                 static_cast<unsigned>(lightCount),
                 static_cast<unsigned>(fps));

    // Facts about the board, not the person. `deviceName` and `mac` sit in the same control list
    // and are deliberately NOT here.
    field(sink, system, "chip", "chip", first);
    field(sink, system, "flash", "flash", first);
    field(sink, system, "psramType", "psram", first);
    field(sink, system, "sdk", "sdk", first);
    field(sink, system, "deviceModel", "deviceModel", first);

    // What the user CHOSE to run, by name and role: see reportModules for which modules count and
    // why. A fixed vocabulary from the catalog, never anything a user typed.
    sink.append(",\"modules\":[");
    bool firstModule = true;
    reportModules(sink, root, moduleCount, firstModule);
    sink.append("]");

    sink.append("}");
    sink.flush();
}


class MoonStatsModule : public MoonModule {
public:
    void setup() override {
        std::snprintf(runningVersion_, sizeof(runningVersion_), "%s", kVersion);
        refreshStatus();
        MoonModule::setup();
    }

    /// What this setting exchanges, on the module's own status slot rather than a control of its
    /// own: the standard place for a line of explanation. The privacy policy points here rather than
    /// carrying a list that dates the moment a field changes, and the charts are the detail, so the
    /// line names them instead of repeating them.
    void refreshStatus() {
        if (consent_) clearStatus();
        else setStatus("Off. Switch on to share what hardware you run, once per install or upgrade "
                       "and whenever you press send update: "
                       "the empty charts below are exactly what it contributes to, and what you get "
                       "back. No device name, no addresses, no credentials.");
    }

    void onControlChanged(const char* name) override {
        if (!name) return;
        if (std::strcmp(name, "consent") == 0) { refreshStatus(); return; }
        if (std::strcmp(name, "send update") != 0) return;
        // Retract the previous verdict before forming a new one. Every line below describes THE
        // LAST ATTEMPT, so a stale one is a lie the moment the next press starts: the failure text
        // outlived the failure and told users a report was outstanding long after one had arrived.
        clearOwnStatus();
        // Every outcome says something. A button that sometimes does nothing and never explains why
        // leaves a user unable to tell "it worked" from "it was ignored", which is the state this
        // card was in: the only way to know was to read the database.
        //
        // Consent is re-read rather than assumed: a control write arrives from the API as readily
        // as from the card, and this is the one path where a user action opens a connection.
        if (!consent_) { setOwnStatus("Switch consent on first: nothing is sent while it is off.",
                                      Severity::Warning); return; }
        if (!platform::httpsAvailable()) {
            setOwnStatus("This build cannot send: it was compiled without an HTTPS client.",
                         Severity::Error);
            return;
        }
        if (!platform::networkReady()) {
            setOwnStatus("No network yet. Press send update again once this device is online.",
                         Severity::Warning);
            return;
        }
        if (inApMode()) {
            setOwnStatus("Serving its own access point, so there is no route out. "
                         "Join a network, then press send update.", Severity::Warning);
            return;
        }
        if (sendReport(MoonStatsEvent::Refresh)) {
            setOwnStatus("Sent. The charts below now describe this device as it is now.",
                         Severity::Status);
        } else {
            setOwnStatus("Could not reach the server. Nothing was sent; press send update to try again.",
                         Severity::Error);
        }
    }

    void defineControls() override {
        controls_.clear();
        // A checkbox, not a four-option select: "not now" and "never" both mean nothing is sent,
        // and telling them apart cost a persisted version and a branch in setup() to express a
        // distinction nobody asked for.
        controls_.addControl("consent", consent_);

        // addText + the readonly FLAG, not addReadOnly: ControlType::ReadOnly is excluded from
        // persistence (Control.cpp, isPersistable) and refused on load, so these two came back empty
        // on every boot. reportedVersion empty means a report is due, so a consented device sent a
        // fresh `install` on EVERY reboot: the reports row was overwritten by its primary key, which
        // hid it, while the events table gained a row per boot and the install pie counted reboots.
        //
        // The flag keeps them display-only in the UI, which is the actual intent: bookkeeping a user
        // would break by editing.
        controls_.addText("reportedVersion", reportedVersion_, sizeof(reportedVersion_));
        controls_.setReadOnly(controls_.count() - 1, true);

        // The running version is derived at setup() from a compile-time constant, so it genuinely
        // has nothing to persist.
        controls_.addReadOnly("version", runningVersion_, sizeof(runningVersion_));

        // Send again on demand. The automatic trigger is a version comparison, which says nothing
        // about a setup that changed WITHOUT a version change: someone who reported a bare board,
        // then wired up the fixtures they actually run, has no way to correct what the charts say
        // about them. Pressing this replaces their row rather than adding one, because the server
        // keys on (installationId, version).
        //
        // "send update", not "refresh": the charts below already carry a ⟲ that re-reads them, and
        // two controls both called refresh would be one word for two different jobs. This one
        // SENDS. The event it carries on the wire is still "refresh", which the server and the
        // stored rows already use.
        //
        // A press that cannot send says why (see onControlChanged) rather than doing nothing: the
        // card gave no sign either way, and the only way to tell was to read the database.
        controls_.addButton("send update");
    }

    /// True when a report is due: the user said yes, and the running version differs from the one
    /// that last reported.
    bool reportDue() const {
        if (!consent_) return false;
        return std::strcmp(reportedVersion_, runningVersion_) != 0;
    }

    /// Which kind of report is due.
    MoonStatsEvent dueEvent() const {
        return reportedVersion_[0] == 0 ? MoonStatsEvent::Install : MoonStatsEvent::Upgrade;
    }

    /// The version being replaced, for an upgrade report, or nullptr on a fresh install.
    const char* previousVersion() const {
        return reportedVersion_[0] == 0 ? nullptr : reportedVersion_;
    }

    /// Record that a report was sent. Called on HAND-OFF rather than on success: a lost report
    /// costs one row, where marking only on success would re-send on every boot.
    void markReported() {
        std::snprintf(reportedVersion_, sizeof(reportedVersion_), "%s", runningVersion_);
        // markDirty() alone marks the MODULE dirty; it does not schedule a save. FilesystemModule's
        // tick1s returns before flushing unless noteDirty() has set its pending flag, and nothing on
        // the report path calls it: the pair is what a control write does (HttpServerModule).
        //
        // Without it an INSTALL usually still saved, riding along with the 2 s debounce the consent
        // write left pending, while an UPGRADE boot (consent already on, no control written) left the
        // new version in RAM only, so the next reboot reported the same upgrade again. That is the
        // case this feature exists for.
        markDirty();
        FilesystemModule::noteDirty();
    }

    /// Record the user's answer, the same way a control write does.
    void setConsent(bool yes) { consent_ = yes; markDirty(); refreshStatus(); }

    bool consent() const { return consent_; }

    /// Fake the running version so a test can exercise a real upgrade; `setup()` reads a
    /// compile-time constant, which a test cannot change.
    void setRunningVersionForTest(const char* v) {
        std::snprintf(runningVersion_, sizeof(runningVersion_), "%s", v ? v : "");
    }

    /// This installation's id, or empty without consent. Gated rather than merely unused, so no
    /// caller can obtain one to log or display.
    void installationId(char* out) const {
        if (!out) return;
        if (!consent_) { out[0] = 0; return; }
        mm::installationId(out);
    }

    /// A bounded blocking send on the 1 Hz housekeeping tick, the same shape HueDriver uses for its
    /// bridge poll. `-Wfunction-effects` warns because the base declares the hook MM_NONBLOCKING for
    /// the per-frame case; the warning names a real property rather than a mistake. At most one call
    /// per firmware install, and the guards below return first once a device has reported.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        if (!reportDue()) return;
        // A build without an HTTPS client can never send, so there is nothing to hand off and
        // nothing to mark. Distinct from a failed attempt: retrying costs nothing when no request is
        // ever made, where re-sending after network loss would turn one report into a heartbeat.
        if (!platform::httpsAvailable()) return;
        if (!platform::networkReady()) return;   // nothing to do yet; try again next second
        // Serving our own AP means no route out, so a send would fail and mark itself reported.
        if (inApMode()) return;
        // Wait for a measured frame rate. Scheduler::fps() divides by tickTimeUs_, which is computed
        // only when the first 1-second timing window closes, so a report built inside that window
        // carries fps 0. This tick runs INSIDE it: the automatic report is the one every
        // installation sends, so every install and upgrade row read 0 while the pie showed a number
        // only for the rare user who pressed the button. Same "try again next second" shape as the
        // network guard above, and it costs the report one second on a path that fires once.
        auto* sched = Scheduler::instance();
        if (!sched || sched->fps() == 0) return;
        sendReport(dueEvent());
    }

    /// Asked of the platform rather than pushed in by NetworkModule: a stale copy is a prompt that
    /// appears at the wrong moment.
    bool inApMode() const { return platform::wifiApConnected(); }

private:
    /// Build the report and POST it once. Marked reported on hand-off: re-sending until a server
    /// answers would turn one report into a heartbeat.
    /// `kind` defaults to the automatic install-or-upgrade decision. The button passes Refresh,
    /// which is the one case the version comparison cannot express.
    bool sendReport(MoonStatsEvent kind) {
        char id[kInstallationIdChars + 1] = {};
        installationId(id);
        if (!id[0]) return false;   // no consent, no id, no report

        // Scheduler exposes module(i) rather than the array. 32 is its own capacity, so this
        // cannot truncate a tree it accepted.
        MoonModule* tree[32] = {};
        auto* sched = Scheduler::instance();
        if (!sched) return false;
        uint8_t count = 0;
        for (uint8_t i = 0; i < sched->moduleCount() && count < 32; i++) {
            if (MoonModule* m = sched->module(i)) tree[count++] = m;
        }

        JsonSink body;
        const LightSummary* lights = Drivers::latestSummary();
        // previousVersion ONLY on an upgrade: the server reads its presence as what makes a row an
        // upgrade (worker.js), and previousVersion() returns reportedVersion_, which is non-empty
        // whenever a refresh is pressed. Sending it there would report every refresh as an upgrade
        // from the version already running.
        const char* prev = (kind == MoonStatsEvent::Upgrade) ? previousVersion() : nullptr;
        buildMoonStatsReport(body, tree, count,
                             kind, id, runningVersion_, prev,
                             lights ? lights->lightCount : 0,
                             static_cast<uint32_t>(platform::totalHeap()),
                             static_cast<uint32_t>(platform::freeHeap()),
                             sched->fps());

        // Sent through the container, which owns the address. The response body is discarded, but
        // WHETHER it was accepted is not: the button reports it, so a press is never silent.
        bool sent = false;
        if (auto* cloud = static_cast<const MoonCloudModule*>(parent())) {
            sent = cloud->post("/api/report", body.data());
        }
        // The AUTOMATIC report marks itself either way: nobody is waiting for it, and re-sending
        // until a server answers would turn one report into a heartbeat.
        //
        // A BUTTON press is different, and marking it would lose data. Pressing it before the
        // automatic report has gone out (no network yet at boot, then a failed send) would set
        // reportedVersion to the running version, reportDue() would be false forever, and the
        // install would never be counted: a press the user was TOLD had failed, silently
        // consuming the report they were waiting for.
        if (kind != MoonStatsEvent::Refresh || sent) markReported();
        return sent;
    }

    /// The verdict this module last put on the status slot, or null. Every `send update` outcome
    /// describes THE LAST ATTEMPT, so it has to be retractable: without this the failure text
    /// outlived the failure, and a card kept reporting a send as outstanding long after the next
    /// one had succeeded. Borrowed string literals, compared by address.
    ///
    /// "Clear only MY status", the rule DriverBase records for the same situation: a module that
    /// called clearStatus() unconditionally would wipe a line something else had every right to
    /// show.
    const char* ownStatus_ = nullptr;

protected:
    // Protected, matching DriverBase::setConfigErr / clearConfigErr: the pair is a subclass tool and
    // a test seam, never part of the card's public surface.

    /// Set a verdict and remember it, so the next press can retract exactly this one.
    void setOwnStatus(const char* msg, Severity sev) {
        ownStatus_ = msg;
        setStatus(msg, sev);
    }

    /// Retract this module's verdict, and only this module's.
    void clearOwnStatus() {
        if (ownStatus_) {
            if (status() == ownStatus_) clearStatus();
            ownStatus_ = nullptr;
        }
    }

private:
    bool consent_ = false;
    char reportedVersion_[32] = {};
    char runningVersion_[32] = {};
};

}  // namespace mm
