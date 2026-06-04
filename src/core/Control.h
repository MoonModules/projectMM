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

enum class ControlType : uint8_t {
    Uint8,
    Uint16,
    Int16,      // signed 16-bit. Used by `lengthType` coordinate controls (Layer
                // start/end), where negative values are legal — e.g. a Layer
                // dragged out of the visible area by a future modifier.
    Bool,
    Text,
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
    IPv4        // dotted-quad IP address (ptr → uint8_t[4]). 4 bytes of storage
                // vs ~16 for a "192.168.255.255\0" string. Serializes/parses as
                // the dotted-quad string at the JSON boundary. Used for the
                // static-IP / gateway / subnet / DNS fields in NetworkModule.
};

struct ControlDescriptor {
    void* ptr = nullptr;
    const char* name = nullptr;
    uintptr_t aux = 0;      // Progress: total capacity. Select: pointer to options array.
    ControlType type = ControlType::Uint8;
    int16_t min = 0;   // Uint8/Int16: UI clamp range. Text/Password/ReadOnly: max = bufSize, min unused.
    int16_t max = 255; // Uint16/Select: natural range, UI ignores these fields.
    bool hidden = false;    // UI visibility flag. Set via ControlList::setHidden() after addX().
                            // Persistence ignores this — hidden controls are still saved/loaded
                            // so toggling visibility doesn't lose state.
    bool readonly = false;  // UI editability flag, INDEPENDENT of ControlType. The Text/Password/etc
                            // types are persistable but normally editable; this flag asks the UI
                            // to render the control as display-only (no input affordance). Used for
                            // values that must persist but are pushed by tooling, not edited by
                            // users (e.g. BoardModule.board, which MoonDeck and the web installer
                            // inject via POST /api/control). HTTP writes still succeed — the flag
                            // is a UI rendering hint, not a write gate. Set via setReadOnly().
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

    // c.min/c.max are uint8_t so they can't bound a uint16 range. Persistence
    // and the live setter rely on the natural type range (0..UINT16_MAX) here,
    // not on c.min/c.max.
    void addUint16(const char* name, uint16_t& var) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Uint16, 0, 0};
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

    void addBool(const char* name, bool& var) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Bool, 0, 1};
    }

    void addText(const char* name, char* var, uint8_t bufSize = 16) {
        grow();
        controls_[count_++] = {var, name, 0, ControlType::Text, 0, bufSize};
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

    void addProgress(const char* name, uint32_t& var, uint32_t total) {
        grow();
        controls_[count_++] = {&var, name, total, ControlType::Progress, 0, 0};
    }

    // 4-byte dotted-quad IPv4 address. `var` must point at a uint8_t[4]
    // (octets in network/display order: var[0]=first octet).
    void addIPv4(const char* name, uint8_t* var) {
        grow();
        controls_[count_++] = {var, name, 0, ControlType::IPv4, 0, 0};
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
    // path but pushed by tooling rather than user-edited (e.g. BoardModule.board).
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
