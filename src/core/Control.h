#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

// Dotted-quad parser used by ControlType::IPv4 writes (HttpServerModule
// /api/control, FilesystemModule persistence load, scenario_runner
// set_control) and any external code that needs to validate user-supplied
// IP strings. Returns true and fills out[4] on a clean parse of "A.B.C.D"
// with each octet in 0..255 and exactly three dots; false otherwise. Lives
// in Control.h (next to ControlType::IPv4) so the wire-format converter
// travels with the type definition.
inline bool parseDottedQuad(const char* s, uint8_t out[4]) {
    if (!s) return false;
    int idx = 0;
    const char* p = s;
    while (idx < 4) {
        char* end = nullptr;
        long v = std::strtol(p, &end, 10);
        if (end == p || v < 0 || v > 255) return false;
        out[idx++] = static_cast<uint8_t>(v);
        if (idx == 4) {
            // Trailing junk (e.g. "1.2.3.4x") fails.
            return *end == '\0';
        }
        if (*end != '.') return false;
        p = end + 1;
    }
    return false;  // unreachable
}

// Dotted-quad formatter — the inverse of parseDottedQuad. Caller-owned
// buffer; 16 bytes always fits (longest output is "255.255.255.255\0" =
// 16 chars). Used by every ControlType::IPv4 serializer (live API, type
// defaults, persistence) so the wire format lives in one place.
inline void formatDottedQuad(char out[16], const uint8_t ip[4]) {
    std::snprintf(out, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

// Coerce a string in-place into a valid DNS/mDNS hostname label (RFC 1123):
// keep only [A-Za-z0-9-], turn any run of other characters (spaces, punctuation,
// dots) into a single '-', and strip leading/trailing '-'. Used on the device
// name, which is the single identity behind the mDNS `<name>.local`, the SoftAP
// SSID, and the DHCP hostname — so a user typing "My Living Room!" gets the valid,
// resolvable "My-Living-Room" everywhere rather than a name mDNS would reject.
// Idempotent: an already-valid name is unchanged. Leaves an empty buffer empty
// (every invalid char) — the caller supplies the fallback (SystemModule's MAC name).
// The 63-char RFC label cap is enforced by the caller's buffer (deviceName_ is 24).
inline void sanitizeHostname(char* buf) {
    if (!buf) return;
    char* w = buf;                       // write cursor (compacts in place; w <= read)
    bool pendingDash = false;            // saw invalid char(s); emit one '-' before next keeper
    for (const char* r = buf; *r; ++r) {
        const char c = *r;
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                       || (c >= '0' && c <= '9') || c == '-';
        if (keep) {
            // Never lead with a '-' (RFC: no leading hyphen) — whether from an invalid-
            // char run (pendingDash) or a literal '-' at the start (w == buf).
            if (c == '-' && w == buf) continue;
            // Collapse a run of invalid chars to a single '-', but never lead with one.
            if (pendingDash && w != buf) *w++ = '-';
            pendingDash = false;
            *w++ = c;
        } else {
            pendingDash = true;          // defer — drops a trailing run entirely
        }
    }
    // Trim trailing '-' (RFC: no trailing hyphen). An invalid-char run was dropped via
    // pendingDash, but literal hyphens are kept as written, so e.g. "a--" lands here as
    // "a--" — loop to strip them all, not just one.
    while (w != buf && w[-1] == '-') --w;
    *w = '\0';
}

enum class ControlType : uint8_t {
    Uint8,
    Uint16,
    Int16,      // signed 16-bit. For coordinate-style controls where negative
                // values are legal. (The light domain's grid coordinate type is
                // int16 for this reason.)
    Pin,        // a GPIO number (int8_t storage, -1 = unused/default). Distinct
                // from Int16 so the UI renders a plain number input, not a slider:
                // a GPIO has no meaningful range to drag, and pins span 0..~52
                // across chips. Serializes/parses as a plain integer.
    Bool,
    Text,
    TextArea,   // multi-line text — identical char-buffer storage and parse/persist
                // path to Text; differs only in the UI type string so the front-end
                // renders a resizable <textarea> instead of a single-line input
                // (used for script source and other multi-line fields).
    Password,   // secret text — /api/state serializes it XOR-obfuscated +
                // base64-encoded, not plaintext. Obfuscation only (the XOR key
                // is shared with app.js), so it is trivially reversible.
    ReadOnly,   // display-only text (ptr → char buffer)
    ReadOnlyInt,// display-only signed int (ptr → int8_t, aux → const char* unit
                // suffix e.g. "dBm"). UI renders "<value> <suffix>" verbatim.
                // 1-byte storage where a string would be ~10 bytes — used for
                // RSSI, TX power, future numeric telemetry.
    Select,     // dropdown (ptr → uint8_t index, aux → options array pointer)
    Progress,   // bar with value/total (ptr → uint32_t value, aux = total)
    IPv4,       // dotted-quad IP address (ptr → uint8_t[4]). 4 bytes of storage
                // vs ~16 for a "192.168.255.255\0" string. Serializes/parses as
                // the dotted-quad string at the JSON boundary. Used for the
                // static-IP / gateway / subnet / DNS fields in NetworkModule.
    List,       // a variable list of rows (ptr → a ListSource the owning module
                // implements). Each row serializes a flat summary object plus an
                // optional detail object (the UI shows rows, expands a row to its
                // detail). Read-only today — discovery output, not user-edited.
                // The data lives in the module (a contiguous array it walks in
                // place), NOT copied into the control system: a List adds zero
                // persistent storage beyond the one descriptor pointer, the same
                // "control holds a void* into module-owned data" shape every addX()
                // uses, one level up. (Data-over-objects: no per-row object graph,
                // no allocation on rebuild — see docs/architecture.md hot-path.)
    Button      // a momentary action, not a stored value. The UI renders a button;
                // a click POSTs a value and the module's onUpdate() runs the action.
                // No backing storage (ptr unused) and non-persistable — distinct
                // from Bool, which is an on/off STATE that renders as a toggle and a
                // toggle is the wrong affordance for "do this now" (e.g. rescan).
};

// Forward-declared (defined below the enum) so the descriptor can hold a pointer.
class JsonSink;

// Backing for a ControlType::List control. The module that owns the data (e.g.
// DevicesModule over its device array) implements this; the control descriptor's
// `ptr` points at the implementation. Serialization (writeControlValue) walks
// rowCount() and calls writeRow/writeRowDetail per row — the rows are produced
// straight from the module's contiguous storage, never copied into the control
// system. This is the standard data-source/adapter shape (cf. UITableView's data
// source, Qt's QAbstractItemModel): the view is generic, the data stays with its
// owner. v1 is read-only; an editable variant can add a writeBack later without
// changing this interface's read path.
struct ListSource {
    virtual ~ListSource() = default;
    // Number of rows currently in the list (may change between calls — e.g. a
    // device scan found more). Bounded small (uint8_t) — these are LAN devices,
    // UI list rows, not bulk data.
    virtual uint8_t listRowCount() const = 0;
    // Append the row's SUMMARY as a JSON object — the fields shown in the
    // collapsed row (e.g. {"name":"MM-70BC","ip":"192.168.1.156","type":"projectMM"}).
    virtual void writeListRow(JsonSink& sink, uint8_t row) const = 0;
    // Append the row's DETAIL as a JSON object — the fields shown when the row is
    // expanded. Default: same as the summary (override to show more).
    virtual void writeListRowDetail(JsonSink& sink, uint8_t row) const {
        writeListRow(sink, row);
    }
    // Rebuild the list from persisted JSON. `json` is the full object FilesystemModule
    // loaded; `key` is this control's name — the source parses `json` with the
    // recursive mm::json reader, navigates to `key` (a JSON array of the same
    // row-summary objects writeListRow produced), and repopulates its own storage.
    // Default no-op: a List that doesn't override simply isn't restored. The model
    // owns its (de)serialization — Control.h stays free of the JsonUtil include, and
    // the control system stays generic. Returns true if it took.
    virtual bool restoreList(const char* /*json*/, const char* /*key*/) { return false; }
};

struct ControlDescriptor {
    void* ptr = nullptr;
    const char* name = nullptr;
    uintptr_t aux = 0;      // Progress: total capacity. Select: pointer to options array.
    ControlType type = ControlType::Uint8;
    // int32_t (not int16_t) so the same fields bound every numeric type: Int16's
    // negatives (down to -32768) AND Uint16's full 0..65535 range, which a 16-bit
    // field couldn't hold. Uint8/Uint16/Int16 all carry a real UI slider range
    // here; Text/Password/ReadOnly reuse max as the buffer size (min unused).
    int32_t min = 0;
    int32_t max = 255;
    bool hidden = false;    // UI visibility flag. Set via ControlList::setHidden() after addX().
                            // Persistence ignores this — hidden controls are still saved/loaded
                            // so toggling visibility doesn't lose state.
    bool readonly = false;  // UI editability flag, INDEPENDENT of ControlType. The Text/Password/etc
                            // types are persistable but normally editable; this flag asks the UI
                            // to render the control as display-only (no input affordance). Used for
                            // values that must persist but are pushed by tooling, not edited by
                            // users (e.g. SystemModule.deviceModel, which MoonDeck and the web installer
                            // inject via POST /api/control). HTTP writes still succeed — the flag
                            // is a UI rendering hint, not a write gate. Set via setReadOnly().
    // Optional per-control input validator (Text/Password only; nullptr = accept anything
    // that fits the buffer). applyControlValue calls it on the incoming string BEFORE the
    // write and returns ApplyResult::Malformed on reject, so the check covers EVERY write
    // path — HTTP /api/control, APPLY_OP over serial, persistence load — in one place.
    // A control with a wire-format constraint (e.g. deviceModel's printable-ASCII rule)
    // declares it here, so the rule lives with the control, not with any one transport.
    bool (*validate)(const char* value) = nullptr;
};

class ControlList {
public:
    ~ControlList() { delete[] controls_; }

    ControlList() = default;
    ControlList(const ControlList&) = delete;
    ControlList& operator=(const ControlList&) = delete;
    ControlList(ControlList&&) = delete;
    ControlList& operator=(ControlList&&) = delete;

    void addUint8(const char* name, uint8_t& var, uint8_t min = 0, uint8_t max = 255) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Uint8, min, max};
    }

    // min/max default to the full type range (no UI constraint) when omitted;
    // pass explicit bounds (e.g. addUint16("sampleRate", r, 8000, 48000)) to get
    // a bounded slider in the UI and server-side clamping on write — the same
    // contract as addUint8/addInt16, now that the descriptor's min/max are int32.
    void addUint16(const char* name, uint16_t& var,
                   uint16_t min = 0, uint16_t max = UINT16_MAX) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Uint16, min, max};
    }

    // lengthType (int16_t) — signed wire format so negative values round-trip
    // correctly. min/max default to INT16_MIN/INT16_MAX (no UI constraint) when
    // omitted; pass explicit bounds (e.g. addInt16("width", w, 1, 512)) to get a
    // bounded slider in the UI and server-side clamping on write.
    void addInt16(const char* name, int16_t& var,
                  int16_t min = INT16_MIN, int16_t max = INT16_MAX) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Int16, min, max};
    }

    // A GPIO pin number (int8_t storage — one byte; -1 = unused/default). A GPIO
    // never exceeds ~54 on any ESP32-family chip, so int8 (−128..127) is ample and
    // smaller than int16. Renders as a plain number input, not a slider (see
    // ControlType::Pin): a GPIO has no meaningful range to drag. min/max are the
    // valid-GPIO span (−1..52), used only as a server-side write-clamp guard; the
    // UI keys rendering off the "pin" type string, not the range.
    void addPin(const char* name, int8_t& var, int16_t min = -1, int16_t max = 52) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Pin, min, max};
    }

    void addBool(const char* name, bool& var) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Bool, 0, 1};
    }

    // validate (optional): a per-control input check applied on every write path
    // (see ControlDescriptor::validate). nullptr accepts anything that fits the buffer.
    // bufSize is uint16_t so a large multi-line value (a script source, hundreds of bytes) isn't
    // capped at 255; the descriptor's `max` (int32_t) carries it through the parse path.
    void addText(const char* name, char* var, uint16_t bufSize = 16,
                 bool (*validate)(const char*) = nullptr) {
        grow();
        controls_[count_++] = {var, name, 0, ControlType::Text, 0, bufSize, false, false, validate};
    }

    // Like addText but the UI renders a resizable multi-line <textarea> (e.g. a
    // script source). Same char-buffer storage and parse/persist behaviour as Text.
    void addTextArea(const char* name, char* var, uint16_t bufSize = 16,
                     bool (*validate)(const char*) = nullptr) {
        grow();
        controls_[count_++] = {var, name, 0, ControlType::TextArea, 0, bufSize, false, false, validate};
    }

    // Like addText but the value is a secret: the API serializes it
    // XOR-obfuscated + base64-encoded (not plaintext, but trivially reversible —
    // see ControlType::Password). Writes still set the real value.
    void addPassword(const char* name, char* var, uint8_t bufSize = 32) {
        grow();
        controls_[count_++] = {var, name, 0, ControlType::Password, 0, bufSize};
    }

    void addReadOnly(const char* name, char* var, uint8_t bufSize = 32) {
        grow();
        controls_[count_++] = {var, name, 0, ControlType::ReadOnly, 0, bufSize};
    }

    // 1-byte signed int telemetry (RSSI, TX power, …) with a unit suffix.
    // The suffix is borrowed (caller owns) — pass a string literal.
    void addReadOnlyInt(const char* name, int8_t& var, const char* unit) {
        grow();
        controls_[count_++] = {&var, name, reinterpret_cast<uintptr_t>(unit),
                               ControlType::ReadOnlyInt, 0, 0};
    }

    void addSelect(const char* name, uint8_t& var, const char* const* options, uint8_t optionCount) {
        grow();
        controls_[count_++] = {&var, name, reinterpret_cast<uintptr_t>(options), ControlType::Select, 0, optionCount};
    }

    // A progress bar (value / total). `bytes` true (default) labels it as KB — the
    // heap / flash / filesystem gauges; false labels it as a plain "value / total"
    // count (e.g. a scan position 0..254). The flag rides the descriptor's unused
    // `min` field (Progress has no range), surfaced as "bytes" in the metadata.
    void addProgress(const char* name, uint32_t& var, uint32_t total, bool bytes = true) {
        grow();
        controls_[count_++] = {&var, name, total, ControlType::Progress, bytes ? 1 : 0, 0};
    }

    // 4-byte dotted-quad IPv4 address. `var` must point at a uint8_t[4]
    // (octets in network/display order: var[0]=first octet).
    void addIPv4(const char* name, uint8_t* var) {
        grow();
        controls_[count_++] = {var, name, 0, ControlType::IPv4, 0, 0};
    }

    // A list of rows backed by a ListSource the caller owns (typically the module
    // itself). Read-only in the UI today. No per-row storage here — the source
    // produces rows on demand from the module's own data (see ControlType::List).
    void addList(const char* name, ListSource& source) {
        grow();
        // Non-const ref: restoreList() mutates the source (repopulates its rows on a
        // persistence load), so the control holds a mutable pointer — no const_cast.
        controls_[count_++] = {&source, name, 0, ControlType::List, 0, 0};
    }

    // A momentary action button (ControlType::Button). No backing storage — a click
    // POSTs through to the module's onUpdate(name), which performs the action. Use
    // for "do this now" (rescan, reset, self-test); use addBool for on/off state.
    void addButton(const char* name) {
        grow();
        controls_[count_++] = {nullptr, name, 0, ControlType::Button, 0, 0};
    }

    void clear() { count_ = 0; }
    uint8_t count() const { return count_; }
    const ControlDescriptor& operator[](uint8_t i) const { return controls_[i]; }

    // Flip the hidden flag on a previously-added control. Typical use: call addX() then
    // setHidden(count() - 1, condition). Hidden controls are not rendered in the UI but
    // remain bound for persistence — toggling visibility doesn't lose state.
    void setHidden(uint8_t i, bool hidden) {
        if (i < count_) controls_[i].hidden = hidden;
    }

    // Flip the readonly flag on a previously-added control. Typical use: call addText()
    // then setReadOnly(count() - 1, true) for a value that's persisted via the standard
    // path but pushed by tooling rather than user-edited (e.g. SystemModule.deviceModel).
    // The UI renders the control display-only; HTTP /api/control writes still apply.
    void setReadOnly(uint8_t i, bool readonly) {
        if (i < count_) controls_[i].readonly = readonly;
    }

private:
    ControlDescriptor* controls_ = nullptr;
    uint8_t count_ = 0;
    uint8_t capacity_ = 0;

    void grow() {
        if (count_ < capacity_) return;
        uint8_t newCap = capacity_ == 0 ? 4 : capacity_ * 2;
        auto* newArr = new ControlDescriptor[newCap];
        for (uint8_t i = 0; i < count_; i++) newArr[i] = controls_[i];
        delete[] controls_;
        controls_ = newArr;
        capacity_ = newCap;
    }
};

// ---------------------------------------------------------------------------
// Serialization API — definitions live in Control.cpp.
//
// JsonSink is forward-declared so the 20+ MoonModule headers that include
// Control.h to call addX() don't transitively pull in JsonSink + its
// dependencies. Only the .cpp files that actually serialize (HttpServerModule,
// FilesystemModule) include JsonSink.h directly.
// ---------------------------------------------------------------------------

class JsonSink;

// Wire-format identifier for a control type — "uint8" / "select" / "ipv4" / …
// Used in the type field of `/api/state` and as the JSON-doc cue for the UI.
const char* controlTypeName(ControlType t);

// Whether this type round-trips through FilesystemModule's load/save. False
// for ReadOnly / ReadOnlyInt / Progress (device-derived display values that
// would just get overwritten on the next loop1s).
bool isPersistable(ControlType t);

// Whether `/api/types`'s default-values block should emit a default for this
// type. False for Password (defaults defeat the secret), false for the
// read-only / derived types (no user input to seed).
bool hasDefault(ControlType t);

// Emit just the JSON value fragment — 42, "hi", true, "1.2.3.4". No name,
// no surrounding quotes for the key, no braces. Caller composes the wrapper.
// Password is rendered as plaintext-JSON-string here (the obfuscation step
// is HTTP-API-specific and stays at the writeControls call site).
void writeControlValue(JsonSink& sink, const ControlDescriptor& c);

// Emit the per-type extras that go alongside `value` in `/api/state`:
//   ,"min":N,"max":M   (Uint8 / Int16)
//   ,"options":[…]     (Select)
//   ,"total":N         (Progress)
//   ,"unit":"…"        (ReadOnlyInt)
// No leading comma, no trailing brace — caller's responsibility. Most types
// emit nothing here.
void writeControlMetadata(JsonSink& sink, const ControlDescriptor& c);

// Outcome of applyControlValue. Caller decides what to do with each:
// HttpServerModule maps to 400-with-message; FilesystemModule treats
// non-Ok as "leave existing"; scenario_runner returns false to the caller.
enum class ApplyResult : uint8_t {
    Ok,
    OutOfRange,    // numeric value outside the descriptor's bounds (Strict only)
    Malformed,     // IPv4 string didn't parse, etc.
    ReadOnly,      // tried to write a display-only control
};

// Out-of-range policy for numeric / Select writes. The HTTP API wants
// strict rejection (a bogus client value should surface as a 400 rather
// than silently get clamped); persistence load wants tolerant clamping
// (a stale on-disk value from a schema change should still come close,
// not silently drop to the default-constructed zero).
enum class ApplyPolicy : uint8_t { Strict, Clamp };

// Parse the JSON value at `json[key]` and apply it to the control's storage.
// `json` is the enclosing JSON object's text; the function calls into
// mm::json::parseInt / parseBool / parseString internally to extract the
// right shape per ControlType. Non-Ok results leave the storage untouched.
ApplyResult applyControlValue(const ControlDescriptor& c,
                              const char* json, const char* key,
                              ApplyPolicy policy = ApplyPolicy::Strict);

} // namespace mm
