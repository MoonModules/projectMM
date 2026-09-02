#pragma once

#include "core/MoonModule.h"
#include "core/InputMapping.h"        // InputAction: the target half, shared with the button service
#include "core/FilesystemModule.h"    // noteDirty: schedule the debounced save on a learned bind
#include "platform/platform.h"        // irRead

#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstring>

namespace mm {

/// A core, domain-neutral infrared-receiver peripheral: it decodes a remote on `pin` and drives
/// other modules' controls. The device's infrared *input*, and the same shape as the button service,
/// which drives the same controls from a physical switch.
///
/// **A list of learned codes, not a fixed set of actions.** The first version carried five compiled
/// actions (on/off, brightness up/down, palette next/prev), which made the firmware's opinion the
/// user's ceiling: a remote has twenty keys, and adding a sixth action meant editing an array and
/// reflashing. Now a row IS the binding, so twenty keys are twenty rows, each learned and each
/// pointing wherever the user wants.
///
/// **A fresh service starts empty**, so the first use is "add a row, learn a key, pick a target".
/// Shipping the old five as defaults would need the device catalog to express a list row, and its
/// config push (`planConfigOps`) has only `add` / `set` / `clearChildren`: no row op exists on
/// either side. Worth adding when a board wants pre-bound rows; not worth inventing for a default.
///
/// **Learning any remote, with no code table.** No firmware can carry a table for every remote in
/// the world, so the binding is taught rather than shipped: set a row's `learn` flag and the next
/// decoded code binds to it. That was true of the first version and stays true here; what changed is
/// that you learn onto a row rather than onto one of five fixed slots.
///
/// **How it acts.** Through `Scheduler::setControl`, the one generic control-set primitive that
/// `/api/control`, Improv, MQTT, the WLED bridge and OSC all use. A row names its target as
/// `Module.control`, so a remote press and an OSC message are indistinguishable to whatever they
/// drive. Pointing a row at `Control.switch1` puts the remote on the control surface where every
/// other transport reaches the same switch; `Drivers.on` drives that control directly.
///
/// **Not auto-wired.** Factory-registered like AudioService: a board with a receiver adds it under
/// `Services` through the device catalog, its `pin` carrying that board's infrared GPIO. On the SE16
/// the line shares GPIO 5 with the Ethernet MISO through the board's hardware switch.
///
/// **Prior art:** consumer remotes use the NEC protocol (a 32-bit address+command frame, LSB-first,
/// ~9 ms lead burst); the ESP-IDF RMT peripheral decodes it (the espressif `ir_nec_transceiver`
/// example). The decode itself lives behind `platform::irRead`.
/// @card InfraredService.png
class InfraredService : public MoonModule, public ListSource {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    void defineControls() override {
        controls_.addPin("pin", pin_);
        controls_.addList("codes", *this);
        MoonModule::defineControls();
    }

    void onControlChanged(const char* controlName) override {
        MoonModule::onControlChanged(controlName);
        if (std::strcmp(controlName, "pin") == 0) reportReady();
    }

    void prepare() override { reportReady(); }

    void tick() MM_NONBLOCKING override {
        if (pin_ < 0) return;
        uint32_t code = 0;
        if (platform::irRead(static_cast<uint16_t>(pin_), code)) processCode(code);
    }

    /// The last decoded code (0 = none yet).
    uint32_t latestCode() const { return lastCode_; }

    /// Feed a decoded code as if it arrived from the receiver: the entry host unit tests drive,
    /// since `platform::irRead` is a stub on desktop. Mirrors `DevicesModule::injectPacketForTest`.
    void injectCodeForTest(uint32_t code) { processCode(code); }

    // --- ListSource: the code rows -------------------------------------------------------------

    /// Editable: rows are the whole point of this module, so the UI shows add and delete and the
    /// list API accepts them. Without this a user could see the rows and change nothing.
    bool isEditableList() const override { return true; }

    uint8_t listRowCount() const override { return count_; }

    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        // The code as hex, which is how a remote's frames are read everywhere else, and how a user
        // compares a row against what the status line reported.
        // `learn` is deliberately NOT here: it is transient UI intent (this row is waiting for the
        // next code), and a raw `false` on every row is noise in a summary a user reads at a glance.
        // The detail view carries it, as the button it actually is.
        sink.appendf("{\"id\":%u,\"code\":\"0x%08lX\"",
                     static_cast<unsigned>(r.id), static_cast<unsigned long>(r.code));
        writeInputActionFields(sink, r.action);
        sink.append("}");
    }

    /// The row's EDITABLE fields. `learn` is a BUTTON rather than a value: arming is an action a
    /// user takes ("bind the next key to this row"), not a setting they leave switched on.
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.append("{\"fields\":[{\"name\":\"code\",\"type\":\"text\",\"value\":");
        char codeStr[16];
        std::snprintf(codeStr, sizeof(codeStr), "0x%08lX", static_cast<unsigned long>(r.code));
        sink.writeJsonString(codeStr);
        sink.appendf("},{\"name\":\"learn\",\"type\":\"button\",\"label\":\"%s\"},",
                     r.learn ? "waiting..." : "learn");
        // hasRelease=false: a remote code is a single event with no matching release, so a `set`
        // row would write its value and latch forever.
        writeInputActionDetailFields(sink, r.action, /*hasRelease=*/false);
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
        if (setInputActionField(r->action, field, valueJson)) { markDirty(); return true; }
        if (std::strcmp(field, "learn") == 0) {
            // A button field PATCHes {"field":"learn","value":""}, so the body is never empty and the
            // VALUE is what says whether this is a press. An empty value means the button was
            // clicked, which arms; an explicit true or false lets the API arm or disarm directly.
            char buf[8] = {};
            json::parseString(valueJson, "value", buf, sizeof(buf));
            const bool arm = buf[0] == 0 ? true : json::parseBool(valueJson, "value");
            // One row learns at a time: arming a second would leave two rows waiting for the next
            // code, and the one that got it would be whichever the loop reached first.
            if (arm) for (uint8_t i = 0; i < count_; i++) rows_[i].learn = false;
            r->learn = arm;
            if (arm) setStatus("press a remote key to bind it");
            return true;   // transient UI intent, not persisted state: no markDirty
        }
        if (std::strcmp(field, "code") == 0) {
            // Hex or decimal, so a user can type a code read off another device rather than only
            // learning it. strtoul with base 0 accepts "0x40BF" and "16575".
            //
            // Rejected rather than truncated: a value too long for the buffer would otherwise parse
            // to whatever its first 23 characters spell, which is a DIFFERENT valid code that binds
            // silently. The user then presses the remote and nothing happens, with the row showing a
            // number they never typed. 24 bytes holds any 32-bit code in either base with room over.
            char buf[24] = {};
            json::parseString(valueJson, "value", buf, sizeof(buf));
            // parseString truncates silently, so a filled buffer means the value did not fit and
            // whatever it holds is a prefix rather than what the user typed.
            if (std::strlen(buf) == sizeof(buf) - 1) return false;
            char* end = nullptr;
            errno = 0;
            const unsigned long long parsed = std::strtoull(buf, &end, 0);
            // Trailing junk ("40BF!" or an empty string) means the user did not type a number, and
            // above 32 bits it is not a code this receiver can ever see.
            //
            // strtoULL and ERANGE, not strtoul: `unsigned long` is 32 bits on the ESP32, where
            // strtoul saturates an over-large value to ULONG_MAX and a `> 0xFFFFFFFF` compare then
            // passes. The bound would hold on a 64-bit host and silently fail on the device, which
            // is the half of the range no host test can reach.
            if (end == buf || *end != 0 || errno == ERANGE || parsed > 0xFFFFFFFFULL) return false;
            r->code = static_cast<uint32_t>(parsed);
            claimCode(r->code, r);          // a typed code takes the binding the same way
            markDirty();
            return true;
        }
        return false;
    }

    /// Rebuild the rows from the persisted list.
    bool restoreList(const char* json, const char* key) override {
        count_ = 0;
        const bool ok = mm::json::forEachListElement(json, key,
            [&](const mm::json::JsonDoc& doc, const mm::json::JsonNode* el) {
                if (count_ >= kMaxRows) return;
                Row& r = rows_[count_++];
                r = Row{};
                r.id = nextId_++;
                char codeStr[16] = {};
                mm::json::readString(mm::json::member(doc, el, "code"), codeStr, sizeof(codeStr));
                r.code = static_cast<uint32_t>(std::strtoul(codeStr, nullptr, 0));
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
        return ok;
    }

private:
    /// One binding: a remote code and what it drives. `learn` is transient UI intent (arm this row
    /// for the next code), never persisted, which is why it is not written by restoreList.
    struct Row {
        uint32_t    id = 0;
        uint32_t    code = 0;     ///< the learned frame, 0 = unbound
        bool        learn = false;
        InputAction action{};
    };

    /// Clear `code` from every row except `keep`, so one key binds to exactly one action.
    ///
    /// The dispatch loop fires the FIRST row holding a code and stops, so a duplicate is a row that
    /// can never run: it looks bound in the list, and pressing the key does someone else's action.
    /// The newest binding wins, which is what a user learning a key onto a second row means by it.
    void claimCode(uint32_t code, const Row* keep) {
        if (code == 0) return;
        for (uint8_t i = 0; i < count_; i++)
            if (&rows_[i] != keep && rows_[i].code == code) rows_[i].code = 0;
    }

    Row* find(uint32_t id) {
        for (uint8_t i = 0; i < count_; i++) if (rows_[i].id == id) return &rows_[i];
        return nullptr;
    }

    /// A decoded code: bind it to the armed row, or run whichever row holds it.
    void processCode(uint32_t code) {
        lastCode_ = code;

        for (uint8_t i = 0; i < count_; i++) {
            if (!rows_[i].learn) continue;
            rows_[i].code = code;
            claimCode(code, &rows_[i]);     // one key, one row: the newest binding wins
            rows_[i].learn = false;
            std::snprintf(statusBuf_, sizeof(statusBuf_), "learned 0x%08lX",
                          static_cast<unsigned long>(code));
            setStatus(statusBuf_);
            // A learned code is written straight into the row rather than through setControl, so it
            // schedules its own save: markDirty flags the subtree, noteDirty stamps the debounce
            // timer tick1s watches. Without this the binding could be lost before an unrelated save.
            markDirty();
            FilesystemModule::noteDirty();
            return;
        }

        for (uint8_t i = 0; i < count_; i++) {
            if (rows_[i].code == 0 || rows_[i].code != code) continue;
            // A `set` row needs a release to clear it, and a remote has none: running it would
            // write the value and leave the control latched with nothing able to undo it. Reported
            // rather than ignored, because a row that silently does nothing reads as a broken
            // remote. The editor does not offer the kind here; this is the API path.
            if (rows_[i].action.kind == InputAction::Kind::Set) {
                setStatus("a set row needs a release, which a remote has no way to send",
                          Severity::Warning);
                return;
            }
            // Reported whether or not it worked, and the buffer cleared first so the line reads
            // THIS press rather than the last one. A row pointing at a missing module or an empty
            // pad otherwise looks exactly like a dead remote, which sends the user to the batteries.
            statusBuf_[0] = 0;
            if (runInputAction(rows_[i].action, /*pressed=*/true, statusBuf_, sizeof(statusBuf_))
                || statusBuf_[0])
                setStatus(statusBuf_);
            return;
        }
        std::snprintf(statusBuf_, sizeof(statusBuf_), "received 0x%08lX (unassigned)",
                      static_cast<unsigned long>(code));
        setStatus(statusBuf_);   // status only: nothing persistent changed, so no dirty mark
    }

    /// Report the TRUE receive state, not just "a pin is set": a pin can be set while the RX channel
    /// cannot bind (a busy pin, a bad GPIO), so "pin set" alone would claim ready on a dead
    /// peripheral. Called from prepare() and on a pin change, so the open cost is off the hot path.
    void reportReady() {
        // Unset pin: release any channel still bound to the OLD pin, else it stays armed on a pin
        // the user just cleared. The valid-to-valid change is handled by ensureChannel; the
        // valid-to-unset path never reaches it, so release here.
        if (pin_ < 0) { platform::irStop(); setStatus("set pin to receive", Severity::Warning); return; }
        if (platform::irChannelReady(static_cast<uint16_t>(pin_))) setStatus("ready");
        else setStatus("infrared channel failed to open, pin busy or invalid?", Severity::Error);
    }

    /// Codes one device can bind. A remote has twenty-odd keys and a user rarely maps them all;
    /// twenty-four leaves room for a full handset without the storage mattering.
    static constexpr uint8_t kMaxRows = 24;

    int8_t   pin_ = -1;              ///< receiver GPIO, -1 until a board or user sets it
    Row      rows_[kMaxRows];
    uint8_t  count_ = 0;
    uint32_t nextId_ = 1;
    uint32_t lastCode_ = 0;          ///< last decoded frame (0 = none)
    char     statusBuf_[48] = {};
};

}  // namespace mm
