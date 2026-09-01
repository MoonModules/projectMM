#pragma once

#include "core/ActiveInstance.h"   // the boot-registry seat, so a surface can find this module
#include "core/ControlSurface.h"
#include "core/MoonModule.h"
#include "core/FilesystemModule.h"
#include "core/JsonSink.h"
#include "core/Scheduler.h"
#include "core/JsonUtil.h"
#include "platform/platform.h"

#include <cstdarg>   // setStatusf
#include <cstdio>
#include <algorithm>
#include <cstring>

namespace mm {

/// Puts the device into a named state, and is where anything that wants to do that will live.
///
/// Its first capability is **presets**: a preset is a JSON file, saving one writes a file, selecting
/// one reads it back. That is the whole model, taken from MoonLight's `ModuleLightsControl` and made
/// generic — MoonLight's presets carry effects and modifiers only, ours carry whichever top-level
/// subtrees the user chose to capture.
///
/// Top-level by necessity rather than convention: a preset reaches ACROSS Layouts, Effects, Drivers
/// and Services, so this module cannot be a child of any of them.
///
/// **Not to be confused with `LightPresetsModule`**, which despite the name is a different thing: a
/// library of named channel-role wirings (which channel of a light carries red, which carries pan)
/// that drivers reference per fixture. That is a *fixture profile*; this is a device state.
///
/// The deep dives are under *More info*, below the attribute/method lists:
/// @xref{what-a-preset-captures|what a preset captures, and portability},
/// @xref{why-files|why files rather than slots}.
///
/// @moreinfo
///
/// ## What a preset captures
///
/// The `capture` controls choose which top-level subtrees a save includes, and the file records the
/// choice, so applying one is never a surprise about what it will touch.
///
/// That choice is what decides **portability**. A preset capturing `Effects` alone is a look: effects,
/// modifiers, their settings, and nothing about the hardware — it applies on any board and drives
/// whatever that board has. Adding `Drivers` makes it a device snapshot that carries pin maps and
/// lane counts, which is what you want for cloning a board and NOT what you want for sharing a look.
///
/// MoonLight avoided the question by only ever capturing effects. Making it selectable is the cost
/// of being generic, and the file header is what keeps it honest.
///
/// ## Why files
///
/// `/.config/presets/<name>.json`, one file per preset, free-form names. Deleting a preset is
/// deleting a file; a preset copied onto the device through the File Manager simply appears in the
/// list; backing up presets is copying a folder. Numbered slots would have bought a fixed grid in
/// the UI at the cost of every one of those.
///
/// The bytes inside are exactly what `FilesystemModule` writes for that subtree, so save and restore
/// reuse the engine that already reconciles a live tree against JSON rather than a second serializer
/// that could drift from it.
class ControlModule : public MoonModule, public ListSource {
public:
    static constexpr const char* kPresetDir = "/.config/presets";
    /// The surface is a fixed grid, so a pad has a POSITION rather than a place in a list: slot 14
    /// is slot 14 whether or not anything is in it, and deleting slot 3 does not shuffle slot 4 into
    /// its place. That is the whole difference between a control surface and a list of files, and it
    /// is why the grid renders all kGridCols * kGridRows cells including the empty ones.
    static constexpr uint8_t kGridCols = 8;
    static constexpr uint8_t kGridRows = 8;
    static constexpr uint8_t kMaxPresets = kGridCols * kGridRows;
    static constexpr uint8_t kMaxNameLen = 32;
    /// The top-level subtrees a preset can carry. Names are `typeName()`s, which is what the file
    /// records and what `Scheduler` resolves them back to.
    static constexpr const char* kCapturable[] = {"Layouts", "Effects", "Drivers", "Services"};
    /// What each capturable subtree covers, named after the CONTAINER rather than after a module
    /// inside it: a preset that captures `Effects` reports "effects". Calling it "layer" named the
    /// container's child type, which reads as though the preset held a single Layer. Index-aligned
    /// with kCapturable; the UI maps these to the same emoji the module cards use (ROLE_EMOJI).
    static constexpr const char* kCaptureRole[] = {"layout", "effects", "driver", "service"};
    static constexpr uint8_t kCaptureCount = sizeof(kCapturable) / sizeof(kCapturable[0]);
    static_assert(sizeof(kCapturable) / sizeof(kCapturable[0]) ==
                  sizeof(kCaptureRole) / sizeof(kCaptureRole[0]),
                  "kCapturable and kCaptureRole are index-aligned");
    /// Index of "Effects" within kCapturable — the role a pure look occupies.
    static constexpr uint8_t kEffectsRole = 1;
    static_assert(kCapturable[kEffectsRole][0] == 'E' && kCapturable[kEffectsRole][1] == 'f' &&
                  kCapturable[kEffectsRole][6] == 's', "kEffectsRole must index Effects");

    /// How many faders the bank shows. Fixed; the surfaces this maps onto have 8
    /// (X-Touch) or 9 (nanoKONTROL), so the count becomes a control once a second surface needs it.
    static constexpr uint8_t kFaderCount = 8;
    /// A row of rotary encoders above the pads, mirroring where both the X-Touch and the QCon put
    /// theirs. Unassigned: the binding UI is what gives them targets.
    static constexpr uint8_t kEncoderCount = 8;

    /// The switch row. A desk's channel buttons (mute, solo, select on a Mackie surface) are the
    /// third control type beside a fader and an encoder, and the one an on/off target needs: a
    /// fader can express `on` only as 0 or 255, which is a switch pretending to be a slider.
    /// Unassigned like the others until the target picker lands.
    static constexpr uint8_t kSwitchCount = 8;

    /// The boot ControlModule (exactly one exists). A transport in another module registers itself
    /// as a surface through this, the same static seam AudioService and DevicesModule use, rather
    /// than needing a compile-time pointer to this module's address.
    static ControlModule* active() { return ActiveInstance<ControlModule>::active(); }

    // --- Control surfaces -------------------------------------------------------------------
    //
    // A surface MIRRORS this module's state; it does not own any. Attaching two (a phone running
    // Open Stage Control, a desk on the rack) keeps both correct, because each is a view of one
    // state rather than a peer syncing with the other.

    /// Attach a surface. Idempotent, and it seeds the surface with the current values so a client
    /// that connects mid-show is correct immediately rather than after the first change.
    void addSurface(ControlSurface* s) {
        if (!s) return;
        for (uint8_t i = 0; i < surfaceCount_; i++)
            if (surfaces_[i] == s) return;
        if (surfaceCount_ >= kMaxSurfaces) return;
        surfaces_[surfaceCount_++] = s;
        // Read the targets BEFORE seeding: a surface that connects between ticks would otherwise be
        // sent whatever the mirror last held, which on the very first connect is the boot default
        // rather than what the rig is running. Same correction mirrorToSurfaces does every tick.
        followTargets();
        resendTo(s);
    }

    /// Push EVERY value to one surface, whatever the mirror last sent. For a client that has just
    /// connected: it knows nothing, and change-detection would leave it wrong until something
    /// happened to move. Deliberately not `mirrorToSurfaces`, which is the steady-state path and
    /// would tell every other surface things they already know.
    void resendTo(ControlSurface* s) {
        if (!s) return;
        for (uint8_t i = 0; i < kSwitchCount; i++)
            s->sendValue(SurfaceControl::Switch, i, switches_[i] ? 255 : 0);
        for (uint8_t i = 0; i < kEncoderCount; i++) s->sendValue(SurfaceControl::Encoder, i, encoders_[i]);
        for (uint8_t i = 0; i < kFaderCount; i++)   s->sendValue(SurfaceControl::Fader, i, faders_[i]);
    }

    /// Detach. A surface MUST do this before it is destroyed: mirrorToSurfaces walks this list from
    /// the render thread, and a dangling entry is a use-after-free on the next tick.
    void removeSurface(ControlSurface* s) {
        for (uint8_t i = 0; i < surfaceCount_; i++) {
            if (surfaces_[i] != s) continue;
            surfaces_[i] = surfaces_[--surfaceCount_];
            surfaces_[surfaceCount_] = nullptr;
            return;
        }
    }

    /// A TURN, not a position. A Mackie encoder sends signed detents rather than an absolute value,
    /// so the accumulation has to happen here: a transport that only knows "the user moved it one
    /// click" cannot know where that lands. Clamped, because a knob has no travel limit and a
    /// control does.
    void applyEncoderDelta(uint8_t index, int8_t delta) {
        if (index >= kEncoderCount) return;
        const int v = static_cast<int>(encoders_[index]) + delta;
        encoders_[index] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    /// A hand is on this control. Feedback to it is suppressed while held, or the device fights the
    /// user: a motorised fader driven to the old value while someone is moving it is the classic
    /// failure, and the reason a desk reports touch at all.
    void setTouched(SurfaceControl kind, uint8_t index, bool held) {
        uint32_t* mask = touchMask(kind);
        if (!mask || index >= 32) return;
        if (held) *mask |= (1u << index);
        else      *mask &= ~(1u << index);
    }

    /// Push changed values to every attached surface. Called from tick1s rather than from the write
    /// path on purpose: sending on write would fire on the render thread for every control write,
    /// INCLUDING the ones a surface just made, which is the echo this design avoids by construction.
    /// Sampling also means a value that arrived and left between two samples never bounces.
    void mirrorToSurfaces() {
        if (surfaceCount_ == 0) return;
        followTargets();
        for (uint8_t i = 0; i < kSwitchCount; i++)
            mirrorOne(SurfaceControl::Switch, i, switches_[i] ? 255 : 0, sentSwitches_[i]);
        for (uint8_t i = 0; i < kEncoderCount; i++)
            mirrorOne(SurfaceControl::Encoder, i, encoders_[i], sentEncoders_[i]);
        for (uint8_t i = 0; i < kFaderCount; i++)
            mirrorOne(SurfaceControl::Fader, i, faders_[i], sentFaders_[i]);
    }

    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        mirrorToSurfaces();
    }

    void defineControls() override {
        // The display strip, ABOVE everything: on the desks this mirrors a channel reads display,
        // then buttons, then knob, then fader, and control order is render order. One shared readout
        // rather than a label per control, which is also how the hardware does it: it shows whatever
        // was touched last, so there is no per-cell staleness to reason about. A surface without
        // displays simply ignores it.
        controls_.addReadOnly("display", display_, sizeof(display_));

        // The switch row sits at the TOP, above the encoders, matching the surfaces this mirrors
        // (a channel's buttons are above its knob, which is above its fader). Control order is
        // render order, so the declaration order IS the layout.
        for (uint8_t i = 0; i < kSwitchCount; i++) {
            controls_.addControl(kSwitchNames[i], switches_[i]);
            controls_.setSwitchRow(controls_.count() - 1, true, switchTarget(i));
        }
        // Encoders next: they sit ABOVE the pads on the surfaces this mirrors, and control order is
        // render order.
        for (uint8_t i = 0; i < kEncoderCount; i++) {
            controls_.addControl(kEncoderNames[i], encoders_[i]);
            controls_.setEncoder(controls_.count() - 1, true, encoderTarget(i));
        }
        controls_.addList("presets", *this);
        // The fader bank. Each fader is a plain uint8 control, so it persists, appears in /api/state,
        // and is settable from anywhere the control system reaches — which is what a MIDI surface
        // will bind to later. Rendered as a bank of vertical sliders by the UI.
        for (uint8_t i = 0; i < kFaderCount; i++) {
            controls_.addControl(kFaderNames[i], faders_[i]);
            controls_.setFader(controls_.count() - 1, true, surfaceTarget(i));
        }
        // The save form. All HIDDEN: these are what the pad popup drives, not controls a user reads
        // off the card. Shown on the card they were ambiguous — `name` and the capture toggles look
        // like they describe the selected preset, when they actually describe the NEXT save, and
        // nothing on the card could say which pad they meant. Hidden keeps them settable from the
        // popup, the API and persistence, without pretending to be a second way to do the job.
        controls_.addText("name", name_, sizeof(name_), validPresetName);
        controls_.setHidden(controls_.count() - 1, true);
        // The pad a save is aimed at: transient UI intent, reset at setup (see setup()), never a
        // restored value. The declared max is kMaxPresets-1, so a persisted or API-supplied kNoSlot
        // would clamp to the last pad; savePreset re-checks the range for the same reason.
        controls_.addControl("slot", saveSlot_, 0, kMaxPresets - 1);
        controls_.setHidden(controls_.count() - 1, true);
        // One flag per capturable subtree rather than a single multi-select: the set is small and
        // fixed, and a checkbox each is what makes "what will this preset contain" readable at a
        // glance instead of hidden behind a dropdown.
        // One choice, not four toggles: the UI renders it as a radio group in the pad popup.
        controls_.addSelect("captures", captureRole_, kCapturable, kCaptureCount);
        controls_.setHidden(controls_.count() - 1, true);
        controls_.addButton("save");
        controls_.setHidden(controls_.count() - 1, true);
        MoonModule::defineControls();
    }

    void setup() override {
        // Take the seat before anything looks for us: a transport module registers itself as a
        // surface through active(), and its own setup may run before or after this one.
        seat_.claim();
        platform::fsMkdir(kPresetDir);
        // A restored `slot` is meaningless: it is the popup's "save onto this pad" intent for one
        // save, and the persistence load would clamp a stored kNoSlot to the last pad.
        saveSlot_ = kNoSlot;
        rescan();
        MoonModule::setup();
    }

    /// `save` writes the current state; a fader drives whatever it targets; everything else is a
    /// value edit the base handles.
    void onControlChanged(const char* controlName) override {
        if (std::strcmp(controlName, "save") == 0) { savePreset(); return; }
        for (uint8_t i = 0; i < kFaderCount; i++) {
            if (std::strcmp(controlName, kFaderNames[i]) != 0) continue;
            sentFaders_[i] = faders_[i];   // see markSent
            driveFader(i);
            return;
        }
        for (uint8_t i = 0; i < kEncoderCount; i++) {
            if (std::strcmp(controlName, kEncoderNames[i]) != 0) continue;
            sentEncoders_[i] = encoders_[i];
            driveEncoder(i);
            return;
        }
        for (uint8_t i = 0; i < kSwitchCount; i++) {
            if (std::strcmp(controlName, kSwitchNames[i]) != 0) continue;
            sentSwitches_[i] = switches_[i] ? 255 : 0;
            driveSwitch(i);
            return;
        }
    }

    // ---- ListSource: one row per preset file ----

    uint8_t listRowCount() const override { return presetCount_; }

    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= presetCount_) return;
        const Preset& p = presets_[row];
        // The name is written through writeJsonString, not raw: a preset named with a quote would
        // otherwise emit malformed JSON that fails to parse, and the list would come back empty.
        sink.appendf("{\"id\":%lu,\"slot\":%u,\"name\":",
                     static_cast<unsigned long>(p.id), static_cast<unsigned>(p.slot));
        sink.writeJsonString(p.name);
        sink.append(",\"captures\":");
        sink.writeJsonString(p.captures);
        // The roles this preset covers, as role NAMES: the UI maps them through the same ROLE_EMOJI
        // table the module cards use, so a pad shows 🥞 for a look and 🥞☸️ for one that also
        // carries the hardware — readable at a glance without opening the row.
        sink.append(",\"roles\":[");
        bool firstRole = true;
        for (uint8_t i = 0; i < kCaptureCount; i++) {
            if (!listHas(p.captures, kCapturable[i])) continue;
            sink.appendf("%s\"%s\"", firstRole ? "" : ",", kCaptureRole[i]);
            firstRole = false;
        }
        sink.append("]");
        // Which pad is lit, and for WHICH roles. A pad is active when it still holds at least one
        // role; `activeRoles` is the subset it holds right now, which is what the UI mixes into the
        // lit color. A mixed preset partly superseded (its layer replaced, its layout still on) stays
        // lit for the role it kept.
        sink.append(",\"activeRoles\":[");
        bool firstActive = true;
        for (uint8_t i = 0; i < kCaptureCount; i++) {
            if (!p.name[0] || std::strcmp(p.name, current_[i]) != 0) continue;
            sink.appendf("%s\"%s\"", firstActive ? "" : ",", kCaptureRole[i]);
            firstActive = false;
        }
        sink.append("]");
        if (!firstActive) sink.append(",\"active\":true");
        sink.append("}");
    }

    /// The expanded row: what the preset carries, its name, and the action that applies it.
    ///
    /// `apply` is a `button` field rather than a stored value — the click PATCHes it like an edit and
    /// setListRowField reads the arrival as "put the device into this state". That reuses the list's
    /// existing per-row edit path instead of inventing a second wire shape for actions.
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= presetCount_) return;
        const Preset& p = presets_[row];
        sink.append("{\"fields\":[{\"name\":\"name\",\"type\":\"text\",\"value\":");
        sink.writeJsonString(p.name);
        sink.append("},{\"name\":\"captures\",\"type\":\"text\",\"readonly\":true,\"value\":");
        sink.writeJsonString(p.captures[0] ? p.captures : "(unknown)");
        // refetch: applying a preset rewrites the module tree, so the whole card set is stale.
        sink.append("},{\"name\":\"apply\",\"type\":\"button\",\"label\":\"apply\","
                    "\"refetch\":true}]}");
    }

    // ---- Presets as an external surface (Home Assistant, and any future consumer) --------------
    //
    // A preset carrying ONLY Effects is a look: it changes what the lights show and nothing else. One
    // that also carries Drivers or Layouts rewires pins or geometry, which must not be reachable from
    // a voice assistant or an automation that thinks it is picking a colour scheme. These two calls
    // are the whole seam a publisher needs, so no consumer has to learn the file format or the
    // capture rules to decide what is safe to expose.

    /// The one role this preset carries, or kCaptureCount if the file names none or several. A
    /// multi-capture file is one written by an older build: it is listed but not applied, so the
    /// user can see it and delete it rather than have it silently vanish.
    uint8_t roleOf(uint8_t row) const {
        if (row >= presetCount_) return kCaptureCount;
        uint8_t found = kCaptureCount, n = 0;
        for (uint8_t i = 0; i < kCaptureCount; i++)
            if (listHas(presets_[row].captures, kCapturable[i])) { found = i; n++; }
        return n == 1 ? found : kCaptureCount;
    }

    /// Is this preset a pure look? With one role per preset this is simply "its role is Effects".
    bool isLookOnly(uint8_t row) const { return roleOf(row) == kEffectsRole; }

    /// The preset's name, or null for an out-of-range row.
    const char* presetName(uint8_t row) const {
        return row < presetCount_ ? presets_[row].name : nullptr;
    }

    uint8_t presetCount() const { return presetCount_; }

    /// Monotonic revision of the preset SET, bumped by every save, delete, rename and rescan. The
    /// WLED shim reports it as `info.fs.pmt` (Home Assistant re-fetches /presets.json only when that
    /// value changes) and MqttModule re-announces its effect list on it. A COUNTER rather than a
    /// timestamp: two mutations inside one second must still read as two changes.
    uint32_t presetsRevision() const { return presetsRevision_; }

    /// Apply a LOOK by name. Refuses anything that is not look-only, so an external caller cannot
    /// reconfigure the hardware through this entry point even if it names a preset that would.
    bool applyLookByName(const char* name) {
        if (!name || !name[0]) return false;
        for (uint8_t i = 0; i < presetCount_; i++) {
            if (std::strcmp(presets_[i].name, name) != 0) continue;
            if (!isLookOnly(i)) return false;
            return applyPreset(presets_[i].name);
        }
        return false;
    }

    /// The look applied most recently, or "" when none is. Reports the LAYER role's holder, since a
    /// look is by definition what occupies that role.
    const char* currentLook() const { return current_[kEffectsRole]; }

    bool isEditableList() const override { return true; }

    /// The preset FOLDER is the state; rescan() rebuilds these rows at setup. Persisting the list
    /// would write every row into /.config/ControlModule.json on each save and then discard it on
    /// load, since nothing restores it.
    bool persistsList() const override { return false; }

    /// Presets are triggered far more than they are edited, so the rows render as a grid of pads:
    /// one click applies, and the active one is highlighted. The list's edit/delete/rename ops are
    /// unchanged underneath.
    bool listAsPads() const override { return true; }

    /// The surface's shape. The UI renders exactly this many cells, filling from the rows' `slot`
    /// field, so the empty positions are as real as the occupied ones.
    uint8_t listGridCols() const override { return kGridCols; }
    uint8_t listGridRows() const override { return kGridRows; }

    /// Deleting a row deletes the file. Nothing else holds preset state, so there is no second copy
    /// to keep in step.
    bool deleteListRow(uint32_t id) override {
        for (uint8_t i = 0; i < presetCount_; i++) {
            if (presets_[i].id != id) continue;
            char path[128];
            pathFor(presets_[i].name, path, sizeof(path));
            // Remember the name before the row goes: the active-role slots refer to it by name, and
            // a deleted preset must not keep a pad lit for a file that no longer exists.
            char goneName[kMaxNameLen];
            std::snprintf(goneName, sizeof(goneName), "%s", presets_[i].name);
            const bool ok = platform::fsRemove(path);
            if (ok) clearCurrentIfNamed(goneName);
            setStatusf(ok ? Severity::Status : Severity::Error,
                       ok ? "deleted %s" : "could not delete %s", presets_[i].name);
            rescan();
            return ok;
        }
        return false;
    }

    /// A fader drives its target through Scheduler::setControl, the same domain-neutral primitive IR
    /// and the network bridges use: this module composes against that rather than reaching into
    /// another module's members. Only fader 1 has a target today (the global brightness every driver
    /// scales by); the rest are unassigned and do nothing until a target picker exists.
    /// What a fader drives, as "Module.control", or null when it drives nothing yet. The UI shows
    /// this in the fader's popup, so the answer comes from the module rather than the UI assuming.
    static const char* surfaceTarget(uint8_t index) {
        return index == 0 ? "Drivers.brightness" : nullptr;
    }

    /// What a switch drives, as "Module.control", or null when it drives nothing yet. Switch 1 is
    /// the master on/off every driver honours, the natural partner to fader 1's brightness: the two
    /// controls a lighting desk expects to find first. The rest wait for the target picker, like
    /// the faders.
    static const char* switchTarget(uint8_t index) {
        return index == 0 ? "Drivers.on" : nullptr;
    }

    /// What an encoder drives, as "Module.control", or null when it drives nothing yet. Encoder 1 is
    /// the palette, the third of the global light params after fader 1's brightness and switch 1's
    /// on/off, and an encoder rather than a fader because a palette is a LIST to step through rather
    /// than a level to ride. Palette stays on `Drivers` where it is consumed; the surface reaches
    /// into it, which is why there is no separate lights-control module.
    static const char* encoderTarget(uint8_t index) {
        return index == 0 ? "Drivers.palette" : nullptr;
    }

    /// Drives whatever `switchTarget` declares. A BOOL body, not a number: the target is a bool
    /// control, and parseBool accepts `true`/`1` but not the 255 a byte path would send.
    void driveSwitch(uint8_t index) {
        const char* target = switchTarget(index);
        if (!target || index >= kSwitchCount) return;
        const char* dot = std::strchr(target, '.');
        if (!dot) return;
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char module[24];
        const size_t n = std::min(static_cast<size_t>(dot - target), sizeof(module) - 1);
        std::memcpy(module, target, n);
        module[n] = '\0';
        char body[32];
        std::snprintf(body, sizeof(body), "{\"value\":%s}", switches_[index] ? "true" : "false");
        sched->setControl(module, dot + 1, body);
        // A bool has no option names, so the strip says on or off rather than 1 or 0: the reason
        // the strip exists is that a number is not what a person reads.
        std::snprintf(display_, sizeof(display_), "%s: %s", dot + 1, switches_[index] ? "on" : "off");
    }

    /// Read every bound control back, so a surface FOLLOWS what it drives.
    ///
    /// Driving is only half of a control surface. Without this the surface's own value and its
    /// target's drift apart the moment anything else writes the target: the web UI, a preset
    /// recall, an audio-reactive effect. They also start out of step, because a surface control's
    /// default has never met the target's persisted value: switch1 read `off` at boot on a device
    /// whose `Drivers.on` was on, which is the bug that named this.
    ///
    /// Reads through Scheduler::getControl, the mirror of the setControl that writes, so a binding
    /// costs nothing here. That is what makes this work for the soft-wired controls to come: a
    /// fader pointed at a different target by the user follows it with no new code.
    ///
    /// The TARGET wins on a disagreement. It is the value the device is actually running on, and
    /// the surface is a view of it: a fader showing something the rig is not doing is the failure
    /// this exists to prevent. A write from the surface still takes effect immediately (it goes
    /// straight through driveFader), so this only ever corrects a value nothing on the surface is
    /// currently moving.
    void followTargets() {
        auto* sched = Scheduler::instance();
        if (!sched) return;
        for (uint8_t i = 0; i < kFaderCount; i++) pullTarget(SurfaceControl::Fader, i, faders_[i]);
        // Encoders follow too, now that encoder 1 drives the palette: without this, changing the
        // palette from the web UI, MQTT or OSC leaves the encoder holding a stale index and pushing
        // it to any attached surface. "A surface FOLLOWS what it drives" is this method's contract.
        for (uint8_t i = 0; i < kEncoderCount; i++) pullTarget(SurfaceControl::Encoder, i, encoders_[i]);
        for (uint8_t i = 0; i < kSwitchCount; i++) {
            uint8_t v = switches_[i] ? 255 : 0;
            if (pullTarget(SurfaceControl::Switch, i, v)) switches_[i] = v != 0;
        }
    }

    /// One control's read-back. Splits the target, asks the scheduler, and reports whether `value`
    /// moved so the caller can store it in whatever the control's own storage is.
    bool pullTarget(SurfaceControl kind, uint8_t index, uint8_t& value) {
        const char* target = kind == SurfaceControl::Switch  ? switchTarget(index)
                           : kind == SurfaceControl::Encoder ? encoderTarget(index)
                                                             : surfaceTarget(index);
        if (!target) return false;                    // unassigned: nothing to follow
        const char* dot = std::strchr(target, '.');
        if (!dot) return false;
        auto* sched = Scheduler::instance();
        if (!sched) return false;
        char module[24];
        const size_t n = std::min(static_cast<size_t>(dot - target), sizeof(module) - 1);
        std::memcpy(module, target, n);
        module[n] = '\0';
        uint8_t live = 0;
        if (!sched->getControl(module, dot + 1, live)) return false;
        if (live == value) return false;
        value = live;
        return true;
    }

    /// Drives whatever `surfaceTarget` declares, so the binding is stated ONCE: the popup and the
    /// action cannot disagree, and a fader starts working the moment it gains a target.
    void driveFader(uint8_t index) {
        const char* target = surfaceTarget(index);
        if (!target) return;                          // unassigned
        const char* dot = std::strchr(target, '.');
        if (!dot) return;
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char module[24];
        const size_t n = std::min(static_cast<size_t>(dot - target), sizeof(module) - 1);
        std::memcpy(module, target, n);
        module[n] = '\0';
        char body[32];
        std::snprintf(body, sizeof(body), "{\"value\":%u}", static_cast<unsigned>(faders_[index]));
        sched->setControl(module, dot + 1, body);
        showOnStrip(module, dot + 1, faders_[index]);
    }

    /// Write an encoder's value onto whatever it targets, the same way a fader does.
    ///
    /// Separate from driveFader only because the value comes from a different bank: the write itself
    /// is identical, and both go through `Scheduler::setControl` so the change rebuilds derived state
    /// and persists exactly as a UI edit would. Encoder 1 drives the palette, which is why this
    /// exists at all: without it an encoder recorded a value and drove nothing.
    void driveEncoder(uint8_t index) {
        const char* target = encoderTarget(index);
        if (!target) return;                          // unassigned
        const char* dot = std::strchr(target, '.');
        if (!dot) return;
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char module[24];
        const size_t n = std::min(static_cast<size_t>(dot - target), sizeof(module) - 1);
        std::memcpy(module, target, n);
        module[n] = '\0';
        char body[32];
        std::snprintf(body, sizeof(body), "{\"value\":%u}", static_cast<unsigned>(encoders_[index]));
        sched->setControl(module, dot + 1, body);
        showOnStrip(module, dot + 1, encoders_[index]);
    }

    /// Write what just happened onto the display strip.
    ///
    /// The NAME of a select's option rather than its index, which is the whole point: turning
    /// encoder 1 through the palettes has to read "Rainbow", not "37". Read from the target
    /// control's own descriptor, so this works for any select anyone binds later without a line of
    /// per-module UI code.
    void showOnStrip(const char* module, const char* control, uint8_t value) {
        auto* sched = Scheduler::instance();
        MoonModule* target = sched ? sched->firstByName(module) : nullptr;
        if (!target) return;
        const ControlList& cs = target->controls();
        for (uint8_t i = 0; i < cs.count(); i++) {
            if (std::strcmp(cs[i].name, control) != 0) continue;
            // A Select carries its options in the descriptor: `aux` is the array, `max` the count.
            // Select ONLY: a Palette's aux is a FUNCTION pointer (addPalette takes optionsFn), so
            // reading it as an array of strings would dereference a code address. A palette's number
            // is the honest readout until that seam offers a name by index.
            const bool isSelect = cs[i].type == ControlType::Select && cs[i].aux;
            if (isSelect && value < cs[i].max) {
                const auto* opts = reinterpret_cast<const char* const*>(cs[i].aux);
                std::snprintf(display_, sizeof(display_), "%s: %s", control, opts[value]);
            } else {
                std::snprintf(display_, sizeof(display_), "%s: %u", control,
                              static_cast<unsigned>(value));
            }
            return;
        }
    }

    /// Reorder: the grid can be arranged to match a physical control surface, so pad 3 here is
    /// fader 3 there. The order is the row order, persisted as a `slot` field in each file, so it
    /// survives a reboot and a rescan (which otherwise returns files in whatever order the
    /// filesystem lists them).
    /// Drop a pad onto a grid cell. `to` is the SLOT, not a list index — the UI sends the cell the
    /// pad was dropped on. Dropping onto an occupied slot swaps the two, which is what a user
    /// rearranging a surface expects and avoids a silent overwrite.
    bool moveListRow(uint32_t id, uint8_t to) override {
        if (to >= kMaxPresets) return false;
        Preset* moving = nullptr;
        for (uint8_t i = 0; i < presetCount_; i++) if (presets_[i].id == id) { moving = &presets_[i]; break; }
        if (!moving) return false;
        if (moving->slot == to) return true;

        Preset* occupant = nullptr;
        for (uint8_t i = 0; i < presetCount_; i++)
            if (presets_[i].slot == to && presets_[i].id != id) { occupant = &presets_[i]; break; }

        const uint8_t from = moving->slot;
        moving->slot = to;
        if (occupant) occupant->slot = from;    // swap rather than overwrite
        // Only the one or two presets whose slot changed get rewritten: a reorder used to rewrite
        // EVERY file on the device, which is flash wear proportional to the collection for a
        // two-preset change.
        // Both files must land, or the pair would both claim one cell after the next rescan. On a
        // partial write the in-memory slots go back to where they were, so the surface still matches
        // what is on disk and the user can retry.
        const bool movedOk = writeSlot(*moving);
        const bool occupantOk = !occupant || writeSlot(*occupant);
        if (!movedOk || !occupantOk) {
            moving->slot = from;
            if (occupant) occupant->slot = to;
            if (movedOk) writeSlot(*moving);            // undo the half that did land
            setStatusf(Severity::Error, "could not save the new pad order");
            sortBySlot();
            return false;
        }
        presetsRevision_++;   // the surface changed: consumers caching the list must re-read
        sortBySlot();
        return true;
    }

    /// The row's editable fields carry the two actions a preset row needs. `apply` is a pseudo-field
    /// rather than a stored value: the UI sends it like an edit, and this reads it as "put the device
    /// into this state". That reuses the existing per-row edit path instead of adding a row-button
    /// primitive to the control system for one caller.
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        for (uint8_t i = 0; i < presetCount_; i++) {
            if (presets_[i].id != id) continue;
            // `activate` is the pad click, `apply` the row button — the same action from the two
            // presentations the list can take.
            if (std::strcmp(field, "activate") == 0 || std::strcmp(field, "apply") == 0)
                return applyPreset(presets_[i].name);
            if (std::strcmp(field, "name") == 0) {
                char newName[kMaxNameLen] = {};
                mm::json::parseString(valueJson, "value", newName, sizeof(newName));
                return renamePreset(presets_[i].name, newName);
            }
            return false;
        }
        return false;
    }

private:
    struct Preset {
        uint32_t id = 0;
        char name[kMaxNameLen] = {};
        char captures[64] = {};   // what the file says it carries, shown per row
        uint8_t slot = 0;         // position on the grid (0..kMaxPresets-1), persisted in the file
        bool hasSlot = false;     // false for a file with no stored slot: assignFreeSlots places it
    };

    /// Re-read the folder. Cheap and cold-path: called at setup, and after any save/delete/rename,
    /// so an upload through the File Manager shows up the next time one of those happens.
    void rescan() {
        // Every change to the preset SET funnels through here (save, delete, rename, boot), so this
        // is the one place the revision needs to bump.
        presetsRevision_++;
        presetCount_ = 0;
        platform::fsList(kPresetDir, &onEntry, this);
        for (uint8_t i = 0; i < presetCount_; i++) readCaptures(presets_[i]);
        assignFreeSlots();
        sortBySlot();
    }

    /// A preset with no stored slot (saved before slots existed, or copied in by hand) takes the
    /// first free cell rather than colliding at 0, so a folder of hand-made files still lays out.
    void assignFreeSlots() {
        bool taken[kMaxPresets] = {};
        for (uint8_t i = 0; i < presetCount_; i++)
            if (presets_[i].hasSlot) taken[presets_[i].slot] = true;
        for (uint8_t i = 0; i < presetCount_; i++) {
            if (presets_[i].hasSlot) continue;
            for (uint8_t s = 0; s < kMaxPresets; s++)
                if (!taken[s]) { presets_[i].slot = s; taken[s] = true; break; }
        }
    }

    void sortBySlot() {
        for (uint8_t i = 1; i < presetCount_; i++) {
            Preset key = presets_[i];
            int j = i - 1;
            while (j >= 0 && presets_[j].slot > key.slot) { presets_[j + 1] = presets_[j]; j--; }
            presets_[j + 1] = key;
        }
    }

    static void onEntry(const char* name, bool isDir, uint32_t /*size*/, void* user) {
        auto* self = static_cast<ControlModule*>(user);
        if (isDir || self->presetCount_ >= kMaxPresets) return;
        const size_t len = std::strlen(name);
        if (len < 6 || std::strcmp(name + len - 5, ".json") != 0) return;   // only our files
        // Skip a name too long to hold, rather than truncating it: a truncated name reconstructs a
        // DIFFERENT path in pathFor, so apply, rename and delete would all act on the wrong file (or
        // silently fail). A preset uploaded through the File Manager can carry any name, so this is
        // reachable without going through the validator.
        const size_t stem = len - 5;
        if (stem >= sizeof(Preset::name)) return;
        Preset& p = self->presets_[self->presetCount_];
        std::memcpy(p.name, name, stem);
        p.name[stem] = '\0';
        p.id = ++self->nextId_;
        self->presetCount_++;
    }

    /// Drop any active-role claim held by `name` — called when its file goes away.
    void clearCurrentIfNamed(const char* name) {
        for (uint8_t i = 0; i < kCaptureCount; i++)
            if (std::strcmp(current_[i], name) == 0) current_[i][0] = '\0';
    }

    /// Move an active-role claim to a preset's new name, so a rename does not silently unlight it.
    void renameCurrent(const char* from, const char* to) {
        for (uint8_t i = 0; i < kCaptureCount; i++)
            if (std::strcmp(current_[i], from) == 0)
                std::snprintf(current_[i], sizeof(current_[i]), "%s", to);
    }

    /// A preset name becomes a FILE name, so it must not be able to steer the path. ESP32's
    /// fsTranslate is a bare prefix concatenation with no normalization (unlike the desktop
    /// filesystem, which rejects escapes), so `..` in a name would reach outside the preset folder
    /// on device — and delete and rename write through the same path. Rejecting the separator and
    /// the dot outright is the check the HTTP file routes already apply; this extends it to the
    /// second write path rather than leaving that one guarded only on desktop.
    /// Bound as the `name` control's validator, so EVERY write path runs it: HTTP, the persistence
    /// load, and a scripted set.
    static bool validPresetName(const char* value) {
        if (!value) return false;
        const size_t n = std::strlen(value);
        if (n == 0 || n >= kMaxNameLen) return false;
        for (size_t i = 0; i < n; i++) {
            const unsigned char b = static_cast<unsigned char>(value[i]);
            if (b < 0x20 || b > 0x7E) return false;          // printable ASCII only
            if (b == '/' || b == '\\' || b == '.') return false;   // no separator, no dot-segment
        }
        return true;
    }

    /// Read just the `captures` header so a row can say what it carries without loading the body.
    void readCaptures(Preset& p) {
        char path[128];
        pathFor(p.name, path, sizeof(path));
        char head[192] = {};
        const int n = platform::fsReadAt(path, 0, head, sizeof(head) - 1);
        if (n <= 0) return;
        head[n] = '\0';
        mm::json::parseString(head, "captures", p.captures, sizeof(p.captures));
        p.hasSlot = mm::json::hasKey(head, "slot");
        const int slot = mm::json::parseInt(head, "slot");
        p.slot = (p.hasSlot && slot >= 0 && slot < kMaxPresets) ? static_cast<uint8_t>(slot) : 0;
    }

    /// Persist the pad order by stamping each file with its position. Cheap (one small rewrite per
    /// file) and it keeps the order where the presets themselves live, so a folder copied to another
    /// device brings its layout with it rather than resetting to whatever order that filesystem
    /// happens to list.
    /// Rewrite ONE preset's file with its current slot in the header. Callers pass exactly the
    /// presets a mutation touched; rewriting the whole folder for a two-preset swap is flash wear.
    /// Returns whether the file was written — a caller swapping two slots must not report success
    /// when only one landed, or the pair would both claim the same cell after a rescan.
    bool writeSlot(const Preset& p) {
        char path[128];
        pathFor(p.name, path, sizeof(path));
        const long size = platform::fsSize(path);
        if (size <= 0) return false;
        char* body = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
        if (!body) return false;
        bool ok = false;
        const int n = platform::fsRead(path, body, static_cast<size_t>(size) + 1);
        if (n > 0) {
            body[n] = '\0';
            JsonSink sink;
            // Replace the leading '{' with '{"slot":N,' — the slot is a header field like
            // `captures`, and a rewrite is simpler than an in-place patch of a variable-width int.
            sink.appendf("{\"slot\":%u,", static_cast<unsigned>(p.slot));
            const char* rest = std::strchr(body, '{');
            if (rest) {
                const char* after = rest + 1;
                // Drop any previous slot key so repeated reorders do not accumulate them.
                if (std::strncmp(after, "\"slot\":", 7) == 0) {
                    const char* comma = std::strchr(after, ',');
                    if (comma) after = comma + 1;
                }
                sink.append(after);
                if (!sink.overflowed())
                    ok = platform::fsWriteAtomic(path, sink.data(), sink.size());
            }
        }
        platform::free(body);
        return ok;
    }

    static void pathFor(const char* name, char* out, size_t n) {
        std::snprintf(out, n, "%s/%s.json", kPresetDir, name);
    }

    /// Capture the selected subtrees into one file.
    void savePreset() {
        if (name_[0] == 0) { setStatusf(Severity::Warning, "name the preset first"); return; }
        // Flush first: the persistence engine debounces writes by 2 s, and a preset that captured the
        // debounced-but-unwritten state would be a snapshot of a moment that never existed on disk.
        FilesystemModule::flushPending();

        auto* fs = FilesystemModule::instance();
        auto* sched = Scheduler::instance();
        if (!fs || !sched) { setStatusf(Severity::Error, "not ready"); return; }

        JsonSink sink;
        // A save aimed at a pad carries its slot in the file, exactly as writeSlots writes it, so
        // rescan() places it on the pad the user clicked. Stamping it here rather than fixing it up
        // afterwards avoids a second whole-folder rewrite and avoids displacing whichever preset
        // happened to hold the first free cell. A save that named no pad omits the key, and
        // assignFreeSlots gives it the first free cell instead.
        if (captureRole_ >= kCaptureCount) { setStatusf(Severity::Warning, "choose what to capture"); return; }
        // A pad can hold one preset: saving a DIFFERENT name onto an occupied slot would leave two
        // files claiming the cell, and the grid can render only one of them. Overwriting the same
        // name on its own pad is the normal save-over flow and passes.
        if (saveSlot_ < kMaxPresets) {
            for (uint8_t i = 0; i < presetCount_; i++) {
                if (presets_[i].slot != saveSlot_) continue;
                if (std::strcmp(presets_[i].name, name_) != 0) {
                    setStatusf(Severity::Warning, "pad %u is taken by %s",
                               static_cast<unsigned>(saveSlot_ + 1), presets_[i].name);
                    return;
                }
                break;
            }
        }
        const char* type = kCapturable[captureRole_];
        sink.append("{");
        if (saveSlot_ < kMaxPresets)   // anything else (incl. kNoSlot) means "no pad was chosen"
            sink.appendf("\"slot\":%u,", static_cast<unsigned>(saveSlot_));
        sink.appendf("\"captures\":\"%s\"", type);

        // Each captured subtree is written under a "<TypeName>." key prefix into ONE flat object —
        // the same dotted form the persistence engine already writes, just namespaced. That is what
        // lets applySubtree read it back with nothing more than the prefix it already accepts: no
        // nested-object addressing, no second parser.
        MoonModule* m = findTopLevel(sched, type);
        if (!m) { setStatusf(Severity::Error, "%s is not on this device", type); return; }
        char prefix[24];
        std::snprintf(prefix, sizeof(prefix), "%s.", type);
        sink.append(",");
        if (!fs->saveSubtreeTo(m, sink, prefix)) { setStatusf(Severity::Error, "out of memory saving"); return; }
        sink.append("}");
        if (sink.overflowed()) { setStatusf(Severity::Error, "out of memory saving"); return; }

        char path[128];
        pathFor(name_, path, sizeof(path));
        const bool ok = platform::fsWriteAtomic(path, sink.data(), sink.size());
        setStatusf(ok ? Severity::Status : Severity::Error,
                   ok ? "saved %s" : "could not save %s", name_);
        rescan();   // the file carries its slot, so this places it on the clicked pad
    }

    /// Put the device into a preset's state.
    bool applyPreset(const char* presetName) {
        auto* fs = FilesystemModule::instance();
        auto* sched = Scheduler::instance();
        if (!fs || !sched) return false;

        char path[128];
        pathFor(presetName, path, sizeof(path));
        const long size = platform::fsSize(path);
        if (size <= 0) { setStatusf(Severity::Error, "%s is missing", presetName); return false; }
        char* body = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
        if (!body) { setStatusf(Severity::Error, "out of memory applying"); return false; }
        const int n = platform::fsRead(path, body, static_cast<size_t>(size) + 1);
        if (n <= 0) { platform::free(body); setStatusf(Severity::Error, "could not read %s", presetName); return false; }
        body[n] = '\0';

        char captures[64] = {};
        mm::json::parseString(body, "captures", captures, sizeof(captures));

        // Apply every captured subtree, THEN prepare once. Each applySubtree already builds the
        // subtree it touched; a prepareTree() per capture would walk the whole tree N times, and this
        // runs inline on the render tick.
        // Exactly one capture, or this file is not one we can apply.
        uint8_t role = kCaptureCount, roleCount = 0;
        for (uint8_t i = 0; i < kCaptureCount; i++)
            if (listHas(captures, kCapturable[i])) { role = i; roleCount++; }
        if (roleCount != 1) {
            platform::free(body);
            setStatusf(Severity::Warning, roleCount ? "%s carries several roles — re-save it"
                                                    : "%s carries nothing this build knows", presetName);
            return false;
        }
        const char* type = kCapturable[role];
        MoonModule* m = findTopLevel(sched, type);
        if (!m) {
            platform::free(body);
            setStatusf(Severity::Warning, "%s needs %s, which this device does not have", presetName, type);
            return false;
        }
        char prefix[24];
        std::snprintf(prefix, sizeof(prefix), "%s.", type);
        const bool applied = fs->applySubtree(m, body, prefix);
        platform::free(body);
        if (!applied) { setStatusf(Severity::Error, "could not apply %s", presetName); return false; }

        sched->prepareTree();
        // The preset now holds its role; the other three keep whoever held them, so a layout preset
        // and a layer preset stay lit together.
        std::snprintf(current_[role], sizeof(current_[role]), "%s", presetName);
        setStatusf(Severity::Status, "applied %s", presetName);
        return true;
    }

    bool renamePreset(const char* from, const char* to) {
        if (!validPresetName(to)) return false;   // the new name becomes a file name (see validPresetName)
        if (std::strcmp(from, to) == 0) return true;   // renaming to itself is a no-op, not a failure
        char src[128], dst[128];
        pathFor(from, src, sizeof(src));
        pathFor(to, dst, sizeof(dst));
        // Refuse a rename onto an existing preset: the write would overwrite it and the remove would
        // then delete the source, destroying a preset the user never named. Same refuse-on-collision
        // stance moveListRow takes. >= 0, not > 0: fsSize returns 0 for an EXISTING empty file and
        // -1 for a missing one, and an empty destination is still a preset we must not overwrite.
        if (platform::fsSize(dst) >= 0) {
            setStatusf(Severity::Warning, "%s already exists", to);
            return false;
        }
        const long size = platform::fsSize(src);
        if (size <= 0) return false;
        char* buf = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
        if (!buf) return false;
        const int n = platform::fsRead(src, buf, static_cast<size_t>(size) + 1);
        bool ok = false;
        if (n > 0 && platform::fsWriteAtomic(dst, buf, static_cast<size_t>(n))) {
            // The destination now exists; the rename is only complete once the source is gone.
            // Leaving a failed removal unchecked would show the preset TWICE after the rescan, both
            // claiming the same slot.
            if (platform::fsRemove(src)) {
                renameCurrent(from, to);   // the active look follows its new name
                ok = true;
            } else {
                platform::fsRemove(dst);   // roll back, so a half-rename never ships
            }
        }
        platform::free(buf);
        rescan();
        return ok;
    }

    /// Is `type` in the comma-separated `captures` header? Whole-token match, so "Layer" never
    /// matches "Effects".
    static bool listHas(const char* list, const char* type) {
        const size_t tlen = std::strlen(type);
        for (const char* p = list; *p;) {
            const char* end = std::strchr(p, ',');
            const size_t len = end ? static_cast<size_t>(end - p) : std::strlen(p);
            if (len == tlen && std::strncmp(p, type, tlen) == 0) return true;
            if (!end) break;
            p = end + 1;
        }
        return false;
    }

    static MoonModule* findTopLevel(Scheduler* s, const char* typeName) {
        for (uint8_t i = 0; i < s->moduleCount(); i++) {
            MoonModule* m = s->module(i);
            if (m && std::strcmp(m->typeName(), typeName) == 0) return m;
        }
        return nullptr;
    }

    /// Report through the base's status, the way every module does, so the UI shows it in the card's
    /// status chip with the right severity emoji. setStatus takes a BORROWED pointer, so the text
    /// lives in a member buffer rather than a temporary.
    void setStatusf(Severity sev, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(statusBuf_, sizeof(statusBuf_), fmt, ap);
        va_end(ap);
        setStatus(statusBuf_, sev);
    }

    /// Fader names are their position, so a surface binds to "fader1" rather than to a label a user
    /// might rename. Fixed strings because a control's name is a borrowed pointer.
    static constexpr const char* kFaderNames[kFaderCount] =
        {"fader1", "fader2", "fader3", "fader4", "fader5", "fader6", "fader7", "fader8"};
    static constexpr const char* kEncoderNames[kEncoderCount] =
        {"encoder1", "encoder2", "encoder3", "encoder4", "encoder5", "encoder6", "encoder7", "encoder8"};
    ActiveInstance<ControlModule> seat_{*this};

    /// Attached surfaces. A small fixed array: a rig has a desk and a phone, not thirty.
    static constexpr uint8_t kMaxSurfaces = 4;
    ControlSurface* surfaces_[kMaxSurfaces] = {};
    uint8_t surfaceCount_ = 0;
    /// The last value KNOWN to a surface, so only changes go out. Written in two places, and both
    /// are needed: after a push in mirrorOne, and in onControlChanged, which fires for every write
    /// whatever made it. That second one is the echo guard. Without it a value a surface just sent
    /// us looks like a change at the next sample and is sent straight back: a fader dragged over
    /// two seconds gets last second's position pushed back under the user's finger.
    uint8_t sentSwitches_[kSwitchCount] = {};
    uint8_t sentEncoders_[kEncoderCount] = {};
    uint8_t sentFaders_[kFaderCount] = {};
    /// One bit per control, per bank: a hand is on it. See setTouched.
    uint32_t touchedSwitches_ = 0, touchedEncoders_ = 0, touchedFaders_ = 0;

    uint32_t* touchMask(SurfaceControl kind) {
        switch (kind) {
            case SurfaceControl::Switch:  return &touchedSwitches_;
            case SurfaceControl::Encoder: return &touchedEncoders_;
            case SurfaceControl::Fader:   return &touchedFaders_;
            default: return nullptr;   // a pad has no travel to fight over
        }
    }

    /// Push one control if it changed and no hand is on it. `sent` is updated only when the value
    /// actually goes out, so a touched control resyncs on release rather than being lost.
    void mirrorOne(SurfaceControl kind, uint8_t index, uint8_t value, uint8_t& sent) {
        if (value == sent) return;
        const uint32_t* mask = touchMask(kind);
        if (mask && index < 32 && (*mask & (1u << index))) return;
        for (uint8_t s = 0; s < surfaceCount_; s++) surfaces_[s]->sendValue(kind, index, value);
        sent = value;
    }

    static constexpr const char* kSwitchNames[kSwitchCount] =
        {"switch1", "switch2", "switch3", "switch4", "switch5", "switch6", "switch7", "switch8"};
    char    display_[32] = {};   ///< the strip: what the surface last touched, in words
    uint8_t faders_[kFaderCount] = {};
    uint8_t encoders_[kEncoderCount] = {};
    /// bool, not uint8: a switch is on or off, and the UI renders a checkbox from the type. An
    /// on/off target set from a 0..255 fader would be a slider with two useful positions.
    bool switches_[kSwitchCount] = {};
    /// Which pad the next save fills, set by the surface popup. kNoSlot means the save did not name
    /// a pad (a scripted or API save), and assignFreeSlots then places it in the first free cell.
    static constexpr uint8_t kNoSlot = 0xFF;
    uint8_t saveSlot_ = kNoSlot;

    Preset presets_[kMaxPresets];
    uint8_t presetCount_ = 0;
    uint32_t presetsRevision_ = 0;   ///< see presetsRevision(): drives HA's preset re-fetch
    uint32_t nextId_ = 0;
    char name_[kMaxNameLen] = {};
    /// Which ONE subtree the next save captures, as an index into kCapturable. A preset carries
    /// exactly one role: "this preset is a look" is a thing a user can hold in their head, where
    /// "a look and a geometry, lit for one and superseded for the other" is not.
    uint8_t captureRole_ = kEffectsRole;   // a look, by default
    /// Which preset currently holds each capturable role, index-aligned with kCapturable. A
    /// preset that carries layout+layer claims both, so applying a layer-only preset afterwards
    /// replaces the layer holder and leaves the layout one lit. That is what a mixed preset means
    /// here: it owns more than one role, not a special kind of pad.
    char current_[kCaptureCount][kMaxNameLen] = {};
    /// Backing store for the status text: setStatus borrows the pointer, so it must outlive the call.
    char statusBuf_[64] = {};
};

}  // namespace mm
