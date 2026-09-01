#pragma once

#include "core/MoonModule.h"
#include "core/InputMapping.h"   // InputAction: the target half, shared with the infrared service
#include "platform/platform.h"   // gpioInputBegin / gpioRead

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// A core, domain-neutral push-button peripheral: **a list of buttons**, each on its own GPIO, each
/// driving a control. The physical twin of a UI click, and the same shape as the infrared service,
/// which drives the same controls from a remote.
///
/// **A list, because boards have more than one.** A QuinLED Dig-Next-2 has three buttons and a stage
/// rig has a pedalboard; a single pin and a single target was the shape of the first version and it
/// did not survive the second board. Rows are added, edited and deleted at runtime through the
/// generic list machinery, so a board's buttons come from its catalog entry rather than from
/// firmware.
///
/// **How it acts.** Through `Scheduler::setControl`, the one generic control-set primitive that
/// `/api/control`, Improv, MQTT, the WLED bridge and OSC all use. A row names a target as
/// `Module.control`, so a press and an OSC message are indistinguishable to whatever they drive.
/// Pointing a row at `Control.switch1` (the recommended path) puts the button on the control surface
/// where every other transport can also reach it; pointing it at `Drivers.on` drives that control
/// directly. Both are the same mechanism.
///
/// **Momentary vs latching**, because a wall switch and a stage foot pedal want opposite things. A
/// `toggle` row flips its target on each press and ignores the release (press-to-toggle, a light
/// switch); a `set` row writes its value while held and 0 on release (hold-to-activate, a pedal for
/// a burst effect). A foot pedal is electrically a momentary switch on a jack, so it needs no module
/// of its own: it is a row with `kind = set`.
///
/// **Debounced here, not in the platform.** A bouncing contact is a property of the switch, so the
/// seam reports the pad and the module owns the time constant. Polled on tick20ms: a press lasts
/// tens of milliseconds and 50 Hz catches it, where the render tick would sample a switch thousands
/// of times a second to learn the same thing.
///
/// **Not auto-wired.** Factory-registered like AudioService and InfraredService: a board with
/// buttons adds it under `Services` through the device catalog, with a row per button.
/// @card ButtonService.png
class ButtonService : public MoonModule, public ListSource {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    void defineControls() override {
        controls_.addControl("debounceMs", debounceMs_, 1, 200);
        controls_.addList("buttons", *this);
        MoonModule::defineControls();
    }

    void setup() override {
        MoonModule::setup();
        beginPins();
    }

    /// Poll every configured button at 50 Hz. Not on tick(): a contact closes for tens of
    /// milliseconds, so the render rate would sample it thousands of times per press.
    void tick20ms() MM_NONBLOCKING override {
        for (uint8_t i = 0; i < count_; i++) pollRow(rows_[i]);
    }

    // --- ListSource: the button rows -----------------------------------------------------------

    /// Editable: rows are the whole point of this module, so the UI shows add and delete and the
    /// list API accepts them. Without this a user could see the rows and change nothing.
    bool isEditableList() const override { return true; }

    uint8_t listRowCount() const override { return count_; }

    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.appendf("{\"id\":%u,\"pin\":%d,\"activeLow\":%s",
                     static_cast<unsigned>(r.id), static_cast<int>(r.pin),
                     r.activeLow ? "true" : "false");
        writeInputActionFields(sink, r.action);
        // The live state, so a user wiring a button can see it work before binding it to anything.
        sink.appendf(",\"pressed\":%s}", r.pressed ? "true" : "false");
    }

    /// The row's EDITABLE fields: what the UI builds inputs from, so a button is retargeted on the
    /// card rather than through the API. The summary above is what a collapsed row shows.
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.appendf("{\"fields\":[{\"name\":\"pin\",\"type\":\"uint8\",\"value\":%d},"
                     "{\"name\":\"activeLow\",\"type\":\"select\",\"value\":%d,"
                     "\"options\":[\"active high\",\"active low\"]},",
                     static_cast<int>(r.pin), r.activeLow ? 1 : 0);
        writeInputActionDetailFields(sink, r.action);
        sink.append("]}");
    }

    /// The target-type options, shared across every row rather than repeated in each.
    void writeListOptionSets(JsonSink& sink) const override { writeInputTargetOptions(sink); }

    bool addListRow(uint32_t& outId) override {
        if (count_ >= kMaxRows) return false;
        Row& r = rows_[count_++];
        r = Row{};
        r.id = nextId_++;
        outId = r.id;
        markDirty();
        return true;
    }

    bool deleteListRow(uint32_t id) override {
        for (uint8_t i = 0; i < count_; i++) {
            if (rows_[i].id != id) continue;
            for (uint8_t j = i; j + 1 < count_; j++) rows_[j] = rows_[j + 1];
            count_--;
            markDirty();
            return true;
        }
        return false;
    }

    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        Row* r = find(id);
        if (!r) return false;
        // The action fields first (target / kind / value), shared with every input service; then
        // this module's own. The shared half owns exactly those three names.
        if (setInputActionField(r->action, field, valueJson)) { markDirty(); return true; }
        if (std::strcmp(field, "pin") == 0) {
            r->pin = static_cast<int8_t>(json::parseInt(valueJson, "value"));
            beginPin(*r);          // live: entering a GPIO makes the button work now, not at reboot
            markDirty();
            return true;
        }
        if (std::strcmp(field, "activeLow") == 0) {
            // A bool from the API, an option index from the UI select (1 = active low). Both mean
            // the same thing, so both are accepted rather than adding a second field name.
            r->activeLow = json::parseBool(valueJson, "value") || json::parseInt(valueJson, "value") == 1;
            beginPin(*r);          // the pull follows activeLow, so re-open the input
            markDirty();
            return true;
        }
        return false;
    }

    /// Rebuild the rows from the persisted list, then open every pin they name.
    bool restoreList(const char* json, const char* key) override {
        count_ = 0;
        const bool ok = mm::json::forEachListElement(json, key,
            [&](const mm::json::JsonDoc& doc, const mm::json::JsonNode* el) {
                if (count_ >= kMaxRows) return;
                Row& r = rows_[count_++];
                r = Row{};
                r.id = nextId_++;
                r.pin = static_cast<int8_t>(mm::json::readInt(mm::json::member(doc, el, "pin")));
                r.activeLow = mm::json::readBool(mm::json::member(doc, el, "activeLow"));
                mm::json::readString(mm::json::member(doc, el, "target"),
                                     r.action.target, sizeof(r.action.target));
                char kind[16] = {};
                mm::json::readString(mm::json::member(doc, el, "kind"), kind, sizeof(kind));
                r.action.kind = std::strcmp(kind, "set") == 0   ? InputAction::Kind::Set
                              : std::strcmp(kind, "delta") == 0 ? InputAction::Kind::Delta
                                                                : InputAction::Kind::Toggle;
                r.action.value =
                    static_cast<int16_t>(mm::json::readInt(mm::json::member(doc, el, "value")));
            });
        beginPins();   // a restored row is only a row until its pin is opened
        return ok;
    }

private:
    /// One button: where it is wired, what it drives, and the debounce state it carries between
    /// polls. The state is per row because two buttons bounce independently.
    struct Row {
        uint32_t    id = 0;
        int8_t      pin = -1;
        bool        activeLow = true;    ///< a switch to ground with a pull-up: the usual wiring
        InputAction action{};
        bool        pressed = false;     ///< the settled state
        bool        candidate = false;   ///< the level being timed
        uint16_t    sinceChange = 0;     ///< ms the candidate has held
    };

    Row* find(uint32_t id) {
        for (uint8_t i = 0; i < count_; i++) if (rows_[i].id == id) return &rows_[i];
        return nullptr;
    }

    void beginPins() { for (uint8_t i = 0; i < count_; i++) beginPin(rows_[i]); }

    /// Open one row's pin as an input. Pull-up for the common wiring (a switch to ground),
    /// pull-down when the switch feeds 3V3 instead, so `activeLow` picks the arrangement.
    void beginPin(Row& r) {
        if (r.pin < 0) return;
        platform::gpioInputBegin(static_cast<uint8_t>(r.pin),
                                 r.activeLow ? platform::GpioPull::Up : platform::GpioPull::Down);
        r.pressed = r.candidate = false;
        r.sinceChange = 0;
    }

    void pollRow(Row& r) MM_NONBLOCKING {
        if (r.pin < 0) return;
        const bool raw = platform::gpioRead(static_cast<uint8_t>(r.pin)) != r.activeLow;

        // Debounce by TIME, not by a sample count: a count would change meaning if the poll rate
        // ever did. A level that differs from the settled one starts the clock; it has to hold for
        // debounceMs before it counts as a real edge.
        if (raw != r.candidate) { r.candidate = raw; r.sinceChange = 0; return; }
        if (raw == r.pressed)   { r.sinceChange = 0; return; }        // already settled here
        if (r.sinceChange < debounceMs_) { r.sinceChange += 20; return; }

        r.pressed = raw;
        r.sinceChange = 0;
        // Only a `set` row acts on the release, and that is its whole point: it writes while held and
        // clears when let go, which is what a foot pedal needs. A toggle and a delta act on the
        // PRESS alone, or one push of the button would fire twice, walking a delta row 100 -> 125 on
        // the press and 150 on the release.
        if (r.action.kind != InputAction::Kind::Set && !r.pressed) return;
        statusBuf_[0] = 0;   // cleared first, so the test below reads THIS press, not the last one
        // Reported whether it worked or not: a press that reaches a missing module, or a pad with
        // nothing in it, is a misconfiguration the user has to be able to SEE. Silently doing
        // nothing looks identical to a broken switch, and sends them to the wiring instead.
        // Unassigned rows never get here (runInputAction returns false without writing a status),
        // so an untargeted button stays quiet, which is a valid state rather than a fault.
        if (runInputAction(r.action, r.pressed, statusBuf_, sizeof(statusBuf_)) || statusBuf_[0])
            setStatus(statusBuf_);
    }

    /// Buttons one device can carry. Three is the most any catalog board wires (Dig-Next-2, Penta);
    /// eight leaves room for a pedalboard without costing anything meaningful.
    static constexpr uint8_t kMaxRows = 8;

    Row      rows_[kMaxRows];
    uint8_t  count_ = 0;
    uint32_t nextId_ = 1;
    uint8_t  debounceMs_ = 25;     ///< shared: one switch type per board, in practice
    char     statusBuf_[48] = {};
};

}  // namespace mm
