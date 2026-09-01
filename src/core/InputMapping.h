#pragma once

#include "core/Control.h"        // ListSource: a mapping list is a list like any other
#include "core/JsonSink.h"       // writeListRow emits a row as JSON
#include "core/JsonUtil.h"       // parsing a row field, and restoring the persisted list
#include "core/Scheduler.h"      // setControl: the one generic control-set primitive

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// What one physical input does: a target control, and how the input changes it.
///
/// Shared by every input service (button, infrared, later encoder and analog) because the half that
/// differs between them is only how the event is *detected*: a pin edge, a decoded remote code, a
/// detent. What happens next is identical, so it lives here once rather than in each module, which
/// is the same "complexity lives in core, domain modules stay simple" rule the light domain follows.
///
/// **The target is a `Module.control` string**, so an input can drive the control surface
/// (`Control.switch1`, the recommended path: one place shows what the device's controls do, and
/// every transport reaches the same switch) or a module control directly (`Drivers.on`). Both are
/// the same mechanism with no special case; the surface is a recommendation, not a rule.
struct InputAction {
    /// How the input changes its target.
    ///
    /// A toggle cannot be written as a delta: +1 on a 0/1 control clamps 0 to 1 but leaves 1 at 1,
    /// so a second press would do nothing. Hence its own kind rather than a magic delta value.
    enum class Kind : uint8_t {
        Toggle = 0,   ///< read the current value, write its inverse. A light switch.
        Set,          ///< write `value`. A momentary hold writes 1 then 0; a pad writes a slot.
        Delta,        ///< add `value` to the target, clamped to its declared bounds. A nudge.
    };

    char    target[32] = "";      ///< "Module.control", empty for an unassigned row
    Kind    kind = Kind::Toggle;
    int16_t value = 0;            ///< Set: what to write. Delta: the signed nudge. Toggle: unused.

    bool assigned() const { return target[0] != 0; }
};

/// Fire the pad in grid position `slot` on `mod`, the same way clicking that pad does.
///
/// A pad is a ROW on a list that renders as a grid (`ListSource::listAsPads`), not a control, so it
/// is reached through the list rather than by control name. Every such list already publishes each
/// row's `slot` and accepts an `activate` field, so this needs to know nothing about presets: any
/// module that grows a pad grid becomes targetable by every input at once.
///
/// Returns false when the module has no pad list or nothing occupies that position, which the caller
/// reports rather than swallowing: a row bound to an empty pad is a mistake worth seeing.
inline bool firePadRow(MoonModule& mod, uint8_t slot, const char* label,
                       char* outStatus, size_t statusLen) {
    auto& cs = mod.controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (cs[i].type != ControlType::List) continue;
        auto* src = static_cast<ListSource*>(cs[i].ptr);
        if (!src || !src->listAsPads()) continue;
        for (uint8_t row = 0; row < src->listRowCount(); row++) {
            // The row's own summary is the only place a slot is published, so it is read back the
            // way the UI reads it. Rows are few and a press is a human-rate event, so the cost of
            // serializing one row per candidate does not matter here.
            char buf[256];
            JsonSink sink(buf, sizeof(buf));
            src->writeListRow(sink, row);
            if (static_cast<int>(slot) != mm::json::parseInt(buf, "slot")) continue;
            const auto id = static_cast<uint32_t>(mm::json::parseInt(buf, "id"));
            const bool ok = src->setListRowField(id, "activate", "{\"value\":true}");
            if (outStatus) std::snprintf(outStatus, statusLen, ok ? "%s fired" : "%s refused", label);
            return ok;
        }
        if (outStatus) std::snprintf(outStatus, statusLen, "%s is empty", label);
        return false;
    }
    if (outStatus) std::snprintf(outStatus, statusLen, "%s has no pads", label);
    return false;
}

/// Apply an action to whatever it targets, through `Scheduler::setControl`.
///
/// The same primitive `/api/control`, Improv, MQTT, the WLED bridge and OSC all use, so a press and
/// an OSC message are indistinguishable to the control they drive: the change rebuilds derived state
/// and persists identically however it arrived. An input never reaches into another module.
///
/// `pressed` carries the physical state for a Set action (a momentary button writes 1 while held and
/// 0 on release); Toggle and Delta ignore it. Writes a short description of what happened into
/// `outStatus` when it is non-null, which is what a module puts on its status line.
///
/// Returns false when the row is unassigned, malformed, or names something that is not there. The
/// caller reports; this reports nothing itself, because a module owns its own status line.
inline bool runInputAction(const InputAction& a, bool pressed,
                           char* outStatus, size_t statusLen) {
    if (!a.assigned()) return false;

    // "Module.control" split at the dot. A target without one is not addressable, so it is refused
    // rather than half-applied against a module named for the whole string.
    const char* dot = std::strchr(a.target, '.');
    if (!dot || dot == a.target || !dot[1]) {
        if (outStatus) std::snprintf(outStatus, statusLen, "%s: not Module.control", a.target);
        return false;
    }
    char module[24] = {};
    const size_t n = static_cast<size_t>(dot - a.target);
    if (n >= sizeof(module)) {
        if (outStatus) std::snprintf(outStatus, statusLen, "module name too long");
        return false;
    }
    std::memcpy(module, a.target, n);
    const char* control = dot + 1;

    Scheduler* sched = Scheduler::instance();
    if (!sched) return false;
    MoonModule* target = sched->firstByName(module);
    if (!target) {
        if (outStatus) std::snprintf(outStatus, statusLen, "no %s module", module);
        return false;
    }

    // A PAD is not a control: it is a ROW on a pad-grid list, so it cannot be found by name the way
    // switch1 can. `Module.padN` is resolved to the row sitting in grid position N and fired, which
    // is exactly what clicking that pad does. Handled here rather than in each input service so a
    // button, a remote and every later transport reach a pad the same way; and resolved through the
    // generic ListSource rather than through ControlModule, so this works for ANY pad grid a module
    // grows, not only the preset surface.
    if (std::strncmp(control, "pad", 3) == 0 && control[3] >= '0' && control[3] <= '9') {
        // Only on the press: a pad fires once. A release firing it again would re-apply the same
        // preset for no reason, and would make a momentary row unusable on a pad.
        if (!pressed) return false;
        const unsigned padNr = std::strtoul(control + 3, nullptr, 10);
        if (padNr < 1) return false;
        return firePadRow(*target, static_cast<uint8_t>(padNr - 1), a.target, outStatus, statusLen);
    }

    // The target's descriptor, for its current value and its bounds. A Delta without bounds would
    // run past the end of a select; a Toggle needs to know what it is inverting.
    const ControlList& ctrls = target->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        const ControlDescriptor& c = ctrls[i];
        if (std::strcmp(c.name, control) != 0) continue;

        char valueJson[32];
        // Through Scheduler::getControl, which switches on the control's declared TYPE and clamps a
        // wider one rather than truncating it. Reading `c.ptr` as a uint8_t here was wrong: a Uint16
        // holding 300 read back as 44 on a little-endian target, so a delta computed from a base
        // that was never the control's value. That reader already exists and ControlModule uses it;
        // re-implementing it is the duplicated per-type switch the coding standards forbid.
        uint8_t cur8 = 0;
        if (!sched->getControl(module, control, cur8)) return false;
        // getControl answers a Bool as 255 so a surface has one scale for everything; a toggle needs
        // it back as 0/1, which is what the control itself stores.
        const int current = (c.type == ControlType::Bool) ? (cur8 ? 1 : 0) : cur8;
        int next = 0;
        switch (a.kind) {
            case InputAction::Kind::Toggle: next = current == 0 ? 1 : 0; break;
            case InputAction::Kind::Set:    next = pressed ? a.value : 0; break;
            case InputAction::Kind::Delta:  next = current + a.value; break;
        }
        if (next < c.min) next = c.min;
        if (next > c.max) next = c.max;

        // A bool control takes true/false; everything else takes a number. Both go through the same
        // primitive, which is what makes a switch and a palette the same code here.
        if (c.type == ControlType::Bool)
            std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%s}", next ? "true" : "false");
        else
            std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}", next);
        sched->setControl(module, control, valueJson);
        if (outStatus) std::snprintf(outStatus, statusLen, "%s -> %d", a.target, next);
        return true;
    }
    if (outStatus) std::snprintf(outStatus, statusLen, "%s has no %s", module, control);
    return false;
}

/// The action half of a mapping row, as JSON, for a module's `writeListRow`.
///
/// Emitted by every input service so a row reads the same wherever it came from, and so the UI
/// renders one row shape rather than one per module.
inline void writeInputActionFields(JsonSink& sink, const InputAction& a) {
    sink.append(",\"target\":");
    sink.writeJsonString(a.target);
    sink.append(",\"kind\":");
    sink.writeJsonString(a.kind == InputAction::Kind::Toggle ? "toggle"
                       : a.kind == InputAction::Kind::Set    ? "set" : "delta");
    sink.appendf(",\"value\":%d", static_cast<int>(a.value));
}

/// The target TYPES an input can point at, as a shared option set for `writeListOptionSets`.
///
/// A type plus a number rather than one list of every control: the surface has 8 switches, 8
/// encoders and 8 faders, so a single dropdown would be 27 entries to scan and would grow with the
/// surface. Picking "switch" and typing 1 is both shorter and clearer, and it stays right if the
/// surface ever carries 16 of something.
///
/// **Only the surface.** An earlier draft also offered `on`, `brightness` and `palette` directly,
/// and that was a second path to the same place: `switch1` already targets `Drivers.on` and `fader1`
/// targets `Drivers.brightness`, so mapping a button to fader 1 IS controlling brightness. Two ways
/// to say one thing is the split brain the two-step model exists to avoid, and it would leave a user
/// wondering which of the two a given row used.
///
/// A row can still name any control directly through the API (`target` accepts a string), which is
/// the escape hatch for anything the surface does not carry yet.
inline constexpr const char* kTargetTypes[] = {
    "",           // unassigned: a row that drives nothing yet
    "switch",     // Control.switchN, toggled
    "encoder",    // Control.encoderN, nudged
    "fader",      // Control.faderN, nudged
    "pad",        // Control.padN, fired: a preset slot, resolved through ControlModule::firePad
};
inline constexpr uint8_t kTargetTypeCount = sizeof(kTargetTypes) / sizeof(kTargetTypes[0]);

/// Whether a target type is numbered. Every type in the list is, now that the surface is the only
/// destination the editor offers; kept as a named test so a future unnumbered type reads clearly.
inline bool targetTypeIsNumbered(uint8_t type) { return type >= 1 && type < kTargetTypeCount; }

/// Build the stored `Module.control` string from a type and a number.
///
/// The STORED form stays one string, which is what keeps `Drivers.on` and `Control.switch1` the same
/// mechanism with no special case: type and number are how a user edits it, not how it is kept.
inline void composeTarget(char* out, size_t outLen, uint8_t type, uint8_t number) {
    if (type == 0 || type >= kTargetTypeCount) { out[0] = 0; return; }
    std::snprintf(out, outLen, "Control.%s%u", kTargetTypes[type], static_cast<unsigned>(number));
}

/// Read a stored target back into a type and a number, for the editor.
///
/// A target set through the API to something this vocabulary cannot express (any other module and
/// control) reads back as type 0, so the dropdown shows unassigned while the row keeps working. That
/// is the honest reading: the editor cannot represent it, and silently rewriting it would be worse.
inline void decomposeTarget(const char* target, uint8_t& type, uint8_t& number) {
    type = 0;
    number = 1;
    if (!target || !target[0]) return;
    // Anything outside Control.<type><n> reads back as unassigned: a row set through the API to
    // "Drivers.on" keeps working, and the dropdown shows "(none)" because it cannot represent it.
    if (std::strncmp(target, "Control.", 8) != 0) return;
    const char* name = target + 8;
    for (uint8_t i = 1; i < kTargetTypeCount; i++) {
        const size_t len = std::strlen(kTargetTypes[i]);
        if (std::strncmp(name, kTargetTypes[i], len) != 0) continue;
        const char* digits = name + len;
        // DIGITS, not merely something: "Control.switchX" would otherwise report index 0 and
        // re-compose to "Control.switch0", a control that does not exist.
        if (*digits < '0' || *digits > '9') continue;
        type = i;
        number = static_cast<uint8_t>(std::strtoul(digits, nullptr, 10));
        return;
    }
}

/// The target-type options, as the shared set a module's `writeListOptionSets` emits. Once per
/// list rather than per row, which is what `optionsRef` exists for.
inline void writeInputTargetOptions(JsonSink& sink) {
    // The CONTENTS only: the serializer already wrapped this in an object (Control.cpp emits
    // `"optionSets":{` before calling and `}` after). Emitting a brace here produced `{{`, which is
    // invalid JSON, and a single bad list blanked the entire UI: every card, and the nav with them.
    sink.append("\"targets\":[");
    for (uint8_t i = 0; i < kTargetTypeCount; i++) {
        if (i) sink.append(",");
        sink.writeJsonString(kTargetTypes[i][0] ? kTargetTypes[i] : "(none)");
    }
    sink.append("]");
}

/// The action half as EDITABLE detail fields, for a module's `writeListRowDetail`.
///
/// The summary row (`writeInputActionFields`) is what a collapsed row shows; this is what the UI
/// builds inputs from, so a user can retarget a button without the API. Emitted here so both
/// services offer the identical edit shape, and a third inherits it.
///
/// `kind` is a select over the three things an input can do, which is the whole vocabulary: toggle a
/// switch, nudge an encoder or fader, or write a pad.
inline void writeInputActionDetailFields(JsonSink& sink, const InputAction& a) {
    // A dropdown and a number, not a text box: the target must name a real control exactly, and a
    // typo is invisible until the input silently does nothing.
    uint8_t type = 0, number = 1;
    decomposeTarget(a.target, type, number);
    sink.appendf("{\"name\":\"target\",\"type\":\"select\",\"optionsRef\":\"targets\",\"value\":%d},"
                 "{\"name\":\"number\",\"type\":\"uint8\",\"value\":%d},",
                 static_cast<int>(type), static_cast<int>(number));
    sink.appendf("{\"name\":\"kind\",\"type\":\"select\",\"value\":%d,"
                 "\"options\":[\"toggle\",\"set\",\"delta\"]},"
                 "{\"name\":\"value\",\"type\":\"uint8\",\"value\":%d}",
                 static_cast<int>(a.kind), static_cast<int>(a.value));
}

/// Set one action field from a row edit. Returns false for a field this does not own, so a module
/// can try its own fields (a pin, a code) after calling this.
inline bool setInputActionField(InputAction& a, const char* field, const char* valueJson) {
    if (std::strcmp(field, "target") == 0) {
        // A STRING from the API ("Drivers.on", readable in a script or a curl), or a type INDEX from
        // the editor's dropdown. Both write the same stored string, so there is one target format
        // however it was set.
        char buf[sizeof(a.target)] = {};
        json::parseString(valueJson, "value", buf, sizeof(buf));
        if (buf[0]) { std::snprintf(a.target, sizeof(a.target), "%s", buf); return true; }
        uint8_t oldType = 0, number = 1;
        decomposeTarget(a.target, oldType, number);   // keep the number the row already had
        const int type = json::parseInt(valueJson, "value");
        if (type < 0 || type >= kTargetTypeCount) return false;
        composeTarget(a.target, sizeof(a.target), static_cast<uint8_t>(type), number);
        return true;
    }
    if (std::strcmp(field, "number") == 0) {
        // The number half. Editing it re-composes the target, so the two fields cannot disagree:
        // there is no separate stored number to drift from the string.
        uint8_t type = 0, oldNr = 1;
        decomposeTarget(a.target, type, oldNr);
        // A target the editor cannot represent (any control named directly through the API) reads
        // back as type 0, and re-composing from that would CLEAR the string. The spinner is rendered
        // for every row, so touching it would silently unassign a working row.
        if (type == 0) return false;
        const int number = json::parseInt(valueJson, "value");
        if (number < 1 || number > 64) return false;   // the surface's banks are 8; a pad grid is 64
        composeTarget(a.target, sizeof(a.target), type, static_cast<uint8_t>(number));
        return true;
    }
    if (std::strcmp(field, "kind") == 0) {
        // A name or an INDEX: the API takes "toggle" (readable in a script or a curl), and the UI's
        // select sends the option's position. One field, both callers, no second name for it.
        char buf[16] = {};
        json::parseString(valueJson, "value", buf, sizeof(buf));
        if (buf[0] == 0) {
            const int idx = json::parseInt(valueJson, "value");
            if (idx < 0 || idx > 2) return false;
            a.kind = static_cast<InputAction::Kind>(idx);
            return true;
        }
        if (std::strcmp(buf, "toggle") == 0)     a.kind = InputAction::Kind::Toggle;
        else if (std::strcmp(buf, "set") == 0)   a.kind = InputAction::Kind::Set;
        else if (std::strcmp(buf, "delta") == 0) a.kind = InputAction::Kind::Delta;
        else return false;   // an unknown kind is refused rather than silently defaulted
        return true;
    }
    if (std::strcmp(field, "value") == 0) {
        a.value = static_cast<int16_t>(json::parseInt(valueJson, "value"));
        return true;
    }
    return false;
}

}  // namespace mm
