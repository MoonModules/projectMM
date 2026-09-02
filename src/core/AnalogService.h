#pragma once

#include "core/MoonModule.h"
#include "core/InputMapping.h"   // InputAction + runInputLevel: the target half, shared with every input service
#include "core/Scheduler.h"
#include "platform/platform.h"   // adcRead / adcMaxCount

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// A core, domain-neutral ANALOG INPUT peripheral: **a list of ADC pins**, each driving a control
/// with a value rather than an event. The continuous twin of `ButtonService`, which drives the same
/// controls from a contact.
///
/// **An expression pedal is the shape this is built around**, and it is why the rows carry more than
/// a pin. A pedal's usable travel is never the full sweep: it rests at some count and tops out well
/// below full scale, so a raw reading mapped straight through would give a control that never
/// reaches 0 or 255 and jumps at one end. `inMin` / `inMax` name the travel that matters and `invert`
/// covers a pot wired the other way round, which is the difference between a pedal that feels right
/// and one a user has to fight.
///
/// **It writes the surface, not a driver.** A row names a target as `Module.control` and goes
/// through `Scheduler::setControl`, the same primitive every other transport uses, so a pedal and an
/// OSC message are indistinguishable to whatever they drive. Pointing a row at `Control.fader1` (the
/// recommended path) puts the pedal on the control surface where the assignment can then be changed
/// without touching the pedal's own configuration.
///
/// **Smoothed here, not in the platform.** An ADC pin jitters by a few counts even at rest, and a
/// pot adds its own noise, so an unfiltered read would rewrite its target every tick forever. The
/// seam reports raw counts (a jitter figure is a property of what is WIRED, which only this module
/// knows) and the smoothing is an exponential average whose weight a user can see and change. A
/// deadband on top of it is what stops a resting pedal from writing at all.
///
/// **Polled on tick20ms.** A foot moves in tens of milliseconds and 50 Hz follows it comfortably,
/// where the render tick would sample a pedal thousands of times a second to learn the same thing.
/// It also means a busy ADC cannot stutter the lights at the frame rate.
///
/// **Not auto-wired.** Factory-registered like the other services: a board with a pedal jack or an
/// on-board sense divider adds it under `Services`, with a row per pin.
/// @card AnalogService.png
class AnalogService : public MoonModule, public ListSource {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    void defineControls() override {
        // How hard the average pulls toward each new reading, as a percentage: 100 is no smoothing
        // at all (follow the pin exactly), and a low number is a heavy filter that lags. Expressed
        // as a weight rather than a time constant because the sample rate is fixed at 50 Hz, so the
        // two say the same thing and this one needs no arithmetic to understand.
        controls_.addControl("smoothing", smoothing_, 1, 100);
        // How far the smoothed value must move before the target is written, in TARGET units (0..255
        // after mapping). A pedal at rest still jitters, and without this every tick would write a
        // value one different from the last, forever.
        controls_.addControl("deadband", deadband_, 0, 32);
        controls_.addList("inputs", *this);
        MoonModule::defineControls();
    }

    /// Poll every configured pin at 50 Hz. Not on tick(): a foot moves in tens of milliseconds.
    void tick20ms() MM_NONBLOCKING override {
        for (uint8_t i = 0; i < count_; i++) pollRow(rows_[i]);
    }

    // --- ListSource: the analog rows ------------------------------------------------------------

    bool isEditableList() const override { return true; }
    uint8_t listRowCount() const override { return count_; }

    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.appendf("{\"id\":%u,\"pin\":%d,\"inMin\":%u,\"inMax\":%u,\"invert\":%s",
                     static_cast<unsigned>(r.id), static_cast<int>(r.pin),
                     static_cast<unsigned>(r.inMin), static_cast<unsigned>(r.inMax),
                     r.invert ? "true" : "false");
        writeInputActionFields(sink, r.action);
        // The LIVE reading, raw and mapped, so a user calibrating a pedal can see both without
        // binding it to anything first: the raw count is what inMin/inMax are set from, and the
        // mapped value is what the target will receive.
        sink.appendf(",\"raw\":%u,\"value\":%u}",
                     static_cast<unsigned>(r.raw), static_cast<unsigned>(r.mapped));
    }

    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.appendf("{\"fields\":[{\"name\":\"pin\",\"type\":\"uint8\",\"value\":%d},"
                     "{\"name\":\"inMin\",\"type\":\"uint16\",\"value\":%u,\"min\":0,\"max\":%u},"
                     "{\"name\":\"inMax\",\"type\":\"uint16\",\"value\":%u,\"min\":0,\"max\":%u},"
                     "{\"name\":\"invert\",\"type\":\"bool\",\"value\":%s},",
                     static_cast<int>(r.pin),
                     static_cast<unsigned>(r.inMin), static_cast<unsigned>(platform::adcMaxCount()),
                     static_cast<unsigned>(r.inMax), static_cast<unsigned>(platform::adcMaxCount()),
                     r.invert ? "true" : "false");
        writeInputActionDetailFields(sink, r.action);
        sink.append("]}");
    }

    void writeListOptionSets(JsonSink& sink) const override { writeInputTargetOptions(sink); }

    bool addListRow(uint32_t& outId) override {
        if (count_ >= kMaxRows) return false;
        Row& r = rows_[count_++];
        r = Row{};
        r.id = nextId_++;
        // Full scale by default, so a new row works end to end before anyone calibrates it: a pedal
        // that moves something is what tells a user the wiring is right.
        r.inMax = platform::adcMaxCount();
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
        // EVERY edit below changes what the row should be writing, so each clears `sent`. The
        // deadband compares against the last value SENT, so without this an edit that lands within
        // the deadband of the old value is swallowed: retargeting a row, or inverting it, left the
        // new target untouched until the input happened to move far enough. Clearing `sent` makes
        // the next poll write unconditionally, which is what "the configuration changed" means.
        if (setInputActionField(r->action, field, valueJson)) { r->sent = false; markDirty(); return true; }
        if (std::strcmp(field, "pin") == 0) {
            r->pin = static_cast<int8_t>(json::parseInt(valueJson, "value"));
            r->primed = false;      // a new pin starts its average fresh rather than drifting from the old one
            r->sent = false;
            markDirty();
            return true;
        }
        if (std::strcmp(field, "inMin") == 0) {
            r->inMin = clampCount(json::parseInt(valueJson, "value"));
            r->sent = false;
            markDirty();
            return true;
        }
        if (std::strcmp(field, "inMax") == 0) {
            r->inMax = clampCount(json::parseInt(valueJson, "value"));
            r->sent = false;
            markDirty();
            return true;
        }
        if (std::strcmp(field, "invert") == 0) {
            r->invert = json::parseBool(valueJson, "value") || json::parseInt(valueJson, "value") == 1;
            r->sent = false;
            markDirty();
            return true;
        }
        return false;
    }

    bool restoreList(const char* json, const char* key) override {
        count_ = 0;
        const bool ok = mm::json::forEachListElement(json, key,
            [&](const mm::json::JsonDoc& doc, const mm::json::JsonNode* el) {
                if (count_ >= kMaxRows) return;
                Row& r = rows_[count_++];
                r = Row{};
                r.id = nextId_++;
                r.pin = static_cast<int8_t>(mm::json::readInt(mm::json::member(doc, el, "pin")));
                r.inMin = clampCount(mm::json::readInt(mm::json::member(doc, el, "inMin")));
                // An absent inMax reads 0, which would map every reading to the same value and look
                // like a dead pedal. A row that never named one gets full scale, its default.
                const auto* mx = mm::json::member(doc, el, "inMax");
                r.inMax = mx ? clampCount(mm::json::readInt(mx)) : platform::adcMaxCount();
                r.invert = mm::json::readBool(mm::json::member(doc, el, "invert"));
                mm::json::readString(mm::json::member(doc, el, "target"),
                                     r.action.target, sizeof(r.action.target));
                char kind[16] = {};
                mm::json::readString(mm::json::member(doc, el, "kind"), kind, sizeof(kind));
                // SET is the default here, not Toggle: an analog input carries a value, and a pedal
                // that toggled something on every reading would be nonsense.
                r.action.kind = std::strcmp(kind, "toggle") == 0 ? InputAction::Kind::Toggle
                              : std::strcmp(kind, "delta") == 0  ? InputAction::Kind::Delta
                                                                 : InputAction::Kind::Set;
                r.action.value =
                    static_cast<int16_t>(mm::json::readInt(mm::json::member(doc, el, "value")));
            });
        return ok;
    }

private:
    /// One analog input: where it is wired, the travel that matters, what it drives, and the filter
    /// state it carries between polls. The state is per row because two pots are independent.
    struct Row {
        uint32_t    id = 0;
        int8_t      pin = -1;
        uint16_t    inMin = 0;           ///< the raw count the travel STARTS at
        uint16_t    inMax = 0;           ///< and where it ends: set to full scale when a row is added
        bool        invert = false;      ///< a pot wired the other way round
        InputAction action{};
        uint16_t    raw = 0;             ///< the last reading, unfiltered: what a user calibrates from
        uint16_t    smoothed = 0;        ///< the running average, in raw counts
        uint8_t     mapped = 0;          ///< what the target last received
        bool        primed = false;      ///< the average holds a real reading, so it can be trusted
        bool        sent = false;        ///< a value has been written, so `mapped` is a real comparison
    };

    static uint16_t clampCount(int v) {
        if (v < 0) return 0;
        const int max = static_cast<int>(platform::adcMaxCount());
        return static_cast<uint16_t>(v > max ? max : v);
    }

    Row* find(uint32_t id) {
        for (uint8_t i = 0; i < count_; i++) if (rows_[i].id == id) return &rows_[i];
        return nullptr;
    }

    /// Read one row, filter it, map it, and write its target when the value actually moved.
    void pollRow(Row& r) MM_NONBLOCKING {
        if (r.pin < 0) return;
        uint16_t raw = 0;
        if (!platform::adcRead(static_cast<uint8_t>(r.pin), raw)) return;
        r.raw = raw;

        // The FIRST reading is taken whole: seeding the average with zero would make every pedal
        // sweep up from the bottom on boot, writing its target the whole way.
        if (!r.primed) { r.smoothed = raw; r.primed = true; }
        else {
            // An exponential average in integers: new = old + (raw - old) * weight / 100. Written
            // with a signed difference so it converges from both directions; the alternative
            // (weighting the two terms separately) loses the low bits and sticks short of the target.
            const int32_t diff = static_cast<int32_t>(raw) - static_cast<int32_t>(r.smoothed);
            const int32_t step = diff * static_cast<int32_t>(smoothing_) / 100;
            // A step of zero is where an integer exponential average STOPS: within 1/weight of the
            // reading the fraction truncates away and the value sticks a few counts short forever.
            // At the top of a pedal's travel that is the difference between 255 and 253, so full
            // brightness would be unreachable no matter how hard the pedal is pushed. Once the step
            // rounds to nothing the remaining distance is smaller than the filter can express, so
            // taking it whole is both correct and the end of the movement.
            r.smoothed = static_cast<uint16_t>(step == 0 ? raw
                                                        : static_cast<int32_t>(r.smoothed) + step);
        }

        const uint8_t value = mapToTarget(r, r.smoothed);
        // The DEADBAND, in target units: a resting pedal still jitters a count or two, and without
        // this the row would write a new value every tick forever, which is 50 setControl calls a
        // second doing nothing. The first write always goes through, so a row reports where it is.
        if (r.sent) {
            const int delta = static_cast<int>(value) - static_cast<int>(r.mapped);
            if (delta <= static_cast<int>(deadband_) && -delta <= static_cast<int>(deadband_)) return;
        }
        r.mapped = value;
        r.sent = true;
        statusBuf_[0] = 0;   // cleared first, so the report below is about THIS move
        // Through the CONTINUOUS path: an analog row carries a value, where a button carries an
        // event. Reported when something is wrong (a missing module, a pad target), so a
        // misconfigured pedal is visible rather than looking like a broken pot.
        if (!runInputLevel(r.action, value, statusBuf_, sizeof(statusBuf_)) && statusBuf_[0])
            setStatus(statusBuf_, Severity::Warning);
    }

    /// Map a raw count through the row's travel into 0..255, the range every surface control uses.
    static uint8_t mapToTarget(const Row& r, uint16_t raw) {
        uint16_t lo = r.inMin, hi = r.inMax;
        // A reversed pair is a legitimate way to say "inverted", and treating it as an error would
        // reject a calibration a user made by moving the pedal to each end in the order they chose.
        bool flip = r.invert;
        if (lo > hi) { const uint16_t t = lo; lo = hi; hi = t; flip = !flip; }
        // A zero-width travel has no answer: report the bottom rather than dividing by zero.
        if (hi == lo) return 0;
        if (raw <= lo) return flip ? 255 : 0;
        if (raw >= hi) return flip ? 0 : 255;
        const uint32_t span = static_cast<uint32_t>(hi) - lo;
        const uint32_t pos  = static_cast<uint32_t>(raw) - lo;
        const uint8_t v = static_cast<uint8_t>(pos * 255u / span);
        return flip ? static_cast<uint8_t>(255 - v) : v;
    }

    static constexpr uint8_t kMaxRows = 8;
    Row      rows_[kMaxRows];
    uint8_t  count_ = 0;
    uint32_t nextId_ = 1;
    uint8_t  smoothing_ = 30;    ///< percent: a moderate filter that still feels immediate
    uint8_t  deadband_ = 1;      ///< target units: enough to silence a resting pedal's jitter
    char     statusBuf_[64] = {};
};

}  // namespace mm
