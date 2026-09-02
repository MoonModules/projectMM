#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The highest valid GPIO number, the default clamp for addPin's Pin controls.
// The build injects the real per-chip value (-DMM_MAX_GPIO=CONFIG_SOC_GPIO_PIN_COUNT-1
// from the IDF; see esp32/main/CMakeLists.txt). This fallback keeps Control.h core
// and standalone-compilable (no platform include, no build flag required) — it's
// the widest current ESP32-family ceiling, so it never *under*-clamps a real board.
#ifndef MM_MAX_GPIO
#define MM_MAX_GPIO 63
#endif

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

/// The type of a control — selects its storage, its UI widget, and its DMX mapping.
/// Each `controls_.addX(name, var, …)` binds one of these to a class variable by
/// reference. Uint8 (a slider, 0–255) is the preferred default; the non-obvious
/// members are noted per value below. There is no RGB color-picker type — effects
/// use a palette index (a Uint8) instead; `float` and `Coord3D` exist but are used
/// minimally, prefer Uint8.
/// What a `filepath` control tells the UI: {directory, extension, template}. Exactly three, because
/// writeControlMetadata reads all three: a shorter array compiled fine through a bare pointer and
/// was read past its end (caught by ASan). A module owns the storage and the descriptor borrows it,
/// the same way addSelect borrows its options array.
///
/// `extension` may be null to offer every file; `template` may be null to start a new file empty.
using FilePathPick = const char* const[3];

enum class ControlType : uint8_t {
    Uint8,      ///< 1 byte, min/max — a 0–255 slider. The preferred default; DMX-mappable.
    Uint16,     ///< 2 bytes — a number input (universe, port). DMX-mappable.
    Int16,      ///< signed 16-bit, min/max — for coordinate-style controls where negatives
                ///< are legal (the light grid coordinate type is int16). A bounded slider
                ///< (unbounded → a ±percentage slider). DMX-mappable.
    Int32,      ///< signed 32-bit, min/max — where a value genuinely exceeds 16 bits and a
                ///< narrower type would wrap. A MoonLive `int` member is the case that
                ///< introduced it: every script scalar occupies a 4-byte slot. A bounded
                ///< slider, same contract as Int16. DMX-mappable via the range.
    Pin,        ///< a GPIO number (int8_t storage, -1 = unused/default). Distinct from Int16
                ///< so the UI renders a plain number input, not a slider (a GPIO has no
                ///< meaningful drag range; pins span 0..~52). min/max clamp writes server-side.
    Bool,       ///< 1 byte — a toggle. DMX 0/1.
    Text,       ///< char[N] — a text input.
    TextArea,   ///< multi-line text — same storage/persist path as Text, a resizable
                ///< `<textarea>` in the UI (script source and other multi-line fields).
    FilePath,   ///< the NAME of a file, with an editor for its CONTENTS in the UI. Same
                ///< char-buffer storage as Text: the value is a ~40-byte reference, and the
                ///< body moves over /api/file, which is the only route that may exceed the
                ///< request buffer. A separate type rather than a flag on TextArea because the
                ///< two store opposite things (TextArea's value IS the body), and every other
                ///< flag leaves a value's meaning untouched. `aux` points at a
                ///< `const char* const[3]` of {directory, extension, template}: the module says
                ///< where its files live, what to list, and what a NEW file starts out containing.
                ///< All three come from the module, so the UI needs no knowledge of any domain.
    Password,   ///< secret text — /api/state serializes it XOR-obfuscated + base64, not
                ///< plaintext. Obfuscation only (XOR key shared with app.js), trivially
                ///< reversible by design — a first line of defence, not encryption.
    ReadOnly,   ///< display-only text (ptr → char buffer).
    ReadOnlyInt,///< display-only signed int (ptr → int8_t, aux → a unit suffix such as
                ///< "dBm"). Renders `<value> <suffix>`; 1 byte where a string would be
                ///< ~10 (RSSI, TX power). See coding-standards § Prefer integers.
    Select,     ///< dropdown (ptr → uint8_t index, aux → options array pointer). DMX mode.
    Progress,   ///< bar with value/total (ptr → uint32_t value, aux = total).
    IPv4,       ///< dotted-quad IP address (ptr → uint8_t[4]). 4 bytes of storage
                ///< vs ~16 for a "192.168.255.255\0" string. Serializes/parses as
                ///< the dotted-quad string at the JSON boundary. Used for the
                ///< static-IP / gateway / subnet / DNS fields in NetworkModule.
    List,       ///< a variable list of rows (ptr → a ListSource the owning module
                ///< implements). Each row serializes a flat summary object plus an
                ///< optional detail object (the UI shows rows, expands a row to its
                ///< detail). Read-only today — discovery output, not user-edited.
                ///< The data lives in the module (a contiguous array it walks in
                ///< place), NOT copied into the control system: a List adds zero
                ///< persistent storage beyond the one descriptor pointer, the same
                ///< "control holds a void* into module-owned data" shape every addX()
                ///< uses, one level up. (Data-over-objects: no per-row object graph,
                ///< no allocation on rebuild — see docs/architecture.md hot-path.)
    Button,     ///< a momentary action, not a stored value. The UI renders a button;
                ///< a click POSTs a value and the module's onControlChanged() runs the action.
                ///< No backing storage (ptr unused) and non-persistable — distinct
                ///< from Bool, which is an on/off STATE that renders as a toggle and a
                ///< toggle is the wrong affordance for "do this now" (e.g. rescan).
    Palette     ///< a color-palette dropdown (ptr → uint8_t index). Like Select, but
                ///< each option carries its gradient *colors* (16 hex stops) so the UI
                ///< renders a gradient swatch per option, not just a name. The light
                ///< domain supplies the names + swatches via the Palette type; the wire
                ///< shape (options:[{name,colors}]) is serialized in writeControlMetadata.
};

// Forward-declared (defined below the enum) so the descriptor can hold a pointer.
class JsonSink;

// A ControlType::Palette control's options come from the light domain (it owns the palette set
// and the swatch colors). The descriptor's `aux` holds a pointer to this function; core calls it
// to emit the `"options":[{"name":…,"colors":…}, …]` array — core stays palette-agnostic.
using PaletteOptionsFn = void (*)(JsonSink& sink);

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
    // Append SHARED option sets for this list as a JSON object, emitted ONCE per list (not per row)
    // so a select field repeated across many rows references the set by name (`"optionsRef":"<name>"`)
    // instead of inlining the identical options array in every row — a preset list with N channel
    // selects × M rows would otherwise serialise the same role-name array N×M times (the 1 Hz push's
    // bulk). Default: no shared sets (`{}`), so a plain list is unchanged. A source with a repeated
    // select overrides to emit `{"<name>":["opt0","opt1",...]}` and its rows emit `optionsRef`.
    virtual void writeListOptionSets(JsonSink& /*sink*/) const {}
    // Rebuild the list from persisted JSON. `json` is the full object FilesystemModule
    // loaded; `key` is this control's name — the source parses `json` with the
    // recursive mm::json reader, navigates to `key` (a JSON array of the same
    // row-summary objects writeListRow produced), and repopulates its own storage.
    // Default no-op: a List that doesn't override simply isn't restored. The model
    // owns its (de)serialization — Control.h stays free of the JsonUtil include, and
    // the control system stays generic. Returns true if it took.
    virtual bool restoreList(const char* /*json*/, const char* /*key*/) { return false; }

    // Is this list's VALUE worth writing to flash? False for a list whose rows are re-derived at
    // setup from a source that is itself already persistent — a folder of files, the live module
    // tree, the pin map. Persisting such a list writes a large array on every save that the loader
    // then discards (restoreList returns false), which is flash wear for nothing.
    // Default true, so a list that genuinely owns its rows keeps persisting unchanged.
    virtual bool persistsList() const { return true; }

    // --- Editable list (the CRUD extension) -----------------------------------------
    // A ListSource that supports adding / removing / reordering / editing rows. This is
    // the editable-data-grid primitive (the write half of the same data-source/adapter
    // shape UITableView-editing / QAbstractItemModel-with-setData use) — a module that
    // owns a library of named things (light presets, and later custom palettes) mixes it
    // in and gets a full editable list in the UI for free, reused rather than re-built.
    //
    // Rows are addressed by a STABLE id (writeListRow emits it as "id"), NOT the row
    // index: a consumer that references a row (a driver pointing at a preset) survives an
    // add / delete / reorder because the id is invariant. isEditableList() reports whether
    // this source is editable (so the generic serializer/UI know to show the affordances);
    // a plain ListSource stays read-only. Each op returns whether it took, so the API layer
    // maps the result onto an HTTP status.
    virtual bool isEditableList() const { return false; }

    // Render the rows as a GRID OF PADS rather than a stacked list: one uniform button per row,
    // labelled with the row's `name`, clicking it fires the row's `activate` field.
    //
    // For rows that are TRIGGERED far more often than they are edited, a list is the wrong shape: it
    // costs a click to expand before the action is even visible, and it hides which row is currently
    // active. A pad grid is what a MIDI deck uses for the same job, and it is the same affordance
    // whether the rows are a handful of named presets or a dense field of numbered channels.
    //
    // Deliberately domain-neutral, and a PRESENTATION hint only — the rows, their ids and the
    // edit/delete/reorder ops are unchanged, so a pad list is still a list and still editable. A
    // source that opts in should:
    //   - emit `"name"` per row (the pad label; short — a number or a word, not a sentence),
    //   - mark the current row `"active":true` so the UI can highlight it,
    //   - accept an `activate` field in setListRowField (the click).
    // Everything else about the row stays as it was.
    virtual bool listAsPads() const { return false; }

    // The pad grid's shape. Non-zero means a FIXED surface: the UI renders cols x rows cells and
    // places each row at its own `slot`, so an empty cell is a real position rather than an absence.
    // That is what separates a control surface from a list drawn in columns — pad 14 is pad 14
    // whether or not anything is in it, and deleting pad 3 does not slide pad 4 into its place.
    // Zero (the default) keeps the flowing layout: pads in row order, wrapping to the card width.
    virtual uint8_t listGridCols() const { return 0; }
    virtual uint8_t listGridRows() const { return 0; }

    // Append a new row with default values; write the new row's stable id into `outId`.
    // Returns false if the list is full or otherwise refuses (e.g. a read-only source).
    virtual bool addListRow(uint32_t& /*outId*/) { return false; }

    // Remove the row with this stable id. Returns false if no such id, or the row is
    // protected (a seeded read-only entry). A referenced-elsewhere row may still be
    // removed — the reference side degrades (id no longer resolves), it does not block.
    virtual bool deleteListRow(uint32_t /*id*/) { return false; }

    // Move the row with this stable id to position `to` (clamped). Returns false on a
    // bad id. Reorder never changes an id, so references are unaffected.
    virtual bool moveListRow(uint32_t /*id*/, uint8_t /*to*/) { return false; }

    // Set one field of the row with this stable id from a JSON value. `field` is the row
    // field name (e.g. "name", "channels", "ch3"); `valueJson` is the request body the
    // source parses for "value" (same convention as applyControlValue). Returns false on
    // a bad id / unknown field / protected row / malformed value. The source owns which
    // fields are editable and their validation — the primitive stays domain-neutral.
    virtual bool setListRowField(uint32_t /*id*/, const char* /*field*/,
                                 const char* /*valueJson*/) { return false; }
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
    // The value the control was BORN with, for the UI's reset-to-default affordance. Normally the
    // UI reads defaults per module TYPE from /api/types, which probes a fresh instance: correct
    // while a type's controls are fixed, and empty for a module whose controls come from data.
    // A scripted module is exactly that (a MoonLive script declares its own), so the default has
    // to travel with the control instance. INT32_MIN means "none declared", so a control that
    // never sets one costs nothing on the wire and the type-level route is unchanged.
    //
    // The cost of a sentinel rather than a flag: a control whose default IS INT32_MIN cannot say
    // so, and is serialized as having none. Nothing declares one — the value is 2.1 billion below
    // any range a control here carries — and the alternative is a bool on every descriptor for a
    // case that has never occurred. Revisit if one ever does.
    static constexpr int32_t kNoDefault = INT32_MIN;
    int32_t def = kNoDefault;
    bool hidden = false;    // UI visibility flag. Set via ControlList::setHidden() after addX().
                            // Persistence ignores this — hidden controls are still saved/loaded
                            // so toggling visibility doesn't lose state.
    bool persistLabel = false;  // Select-only: persistence writes the option LABEL instead of the
                            // index. For Selects whose options come from live enumeration (a NIC
                            // list, an audio device list) where the index is unstable across boots
                            // but the name is what the user chose. The apply path already accepts
                            // labels (Control.cpp Select apply), so both forms always load.
    bool readonly = false;  // UI editability flag, INDEPENDENT of ControlType. The Text/Password/etc
                            // types are persistable but normally editable; this flag asks the UI
                            // to render the control as display-only (no input affordance). Used for
                            // values that must persist but are pushed by tooling, not edited by
                            // users (e.g. SystemModule.deviceModel, which MoonDeck and the web installer
                            // inject via POST /api/control). HTTP writes still succeed — the flag
                            // is a UI rendering hint, not a write gate. Set via setReadOnly().
    bool advanced = false;  // "Expert only" UI flag, INDEPENDENT of hidden. A permanent property of the
                            // control (a dev/tuning readout or knob a casual user doesn't need); the UI
                            // shows it only when System.expertMode is on. Like `hidden`, it's a pure
                            // rendering hint — the control still persists and still accepts HTTP writes,
                            // so tooling and the API reach it regardless. Set via setAdvanced(). (The
                            // client composes the two: expertMode is one global toggle in SystemModule,
                            // read UI-side, so no module needs to reach into System's state.)
    bool numberField = false;  // Renders a numeric control (Uint8/Uint16/Int16) as a plain NUMBER INPUT,
                            // never a drag-slider — for a value where each integer is a discrete address
                            // (a PHY MDIO address, an I2C address, a channel number), not a magnitude you
                            // sweep. A pure UI rendering hint like hidden/advanced; the value, range, and
                            // persistence are unchanged. Set via setNumberField(). (The Pin type already
                            // renders number-only for the same reason — a GPIO is an identity, not a
                            // magnitude; this extends that to non-Pin numerics without the Pin type's
                            // pin-ownership-map claim.)
    // Appended AFTER the other flags and before `validate`: the two addText-family initializers
    // below are positional, so this field's place in the order is load-bearing.
    bool fader = false;     // Render as a vertical fader (see ControlList::setFader). Presentation only.
    bool encoder = false;   // Render as a rotary encoder (see ControlList::setEncoder).
    bool switchRow = false; // Render in the horizontal switch strip (see ControlList::setSwitchRow).
    bool displayStrip = false; // Render as the full-width alphanumeric readout (setDisplayStrip).
    // LIVE STATE, not configuration: the value is driven continuously by something other than a
    // person (a script sweeping a fader, a sensor reading), so it is never written to flash and a
    // change to it never marks its module dirty. Unlike `hidden`/`advanced`/`readonly` above, this
    // is NOT a rendering hint: it is the one flag that changes what persistence does.
    //
    // Why it exists. A control written at 50 Hz starved the debounce: FilesystemModule waits two
    // seconds after the LAST dirty mark, so a continuous writer re-stamped the timer forever and
    // the file was NEVER written (measured on an ESP32-P4: lastSaved only aged, across minutes).
    // A power cut then lost the value, while a writer just slower than the debounce would have
    // rewritten the file forever. Rate-limiting the mark would have fixed the second case and left
    // the first, because the real question is not how often to save but whether a swept value is
    // configuration at all. A fader a script is driving is closer to a sensor reading than to a
    // setting someone chose, so it is declared as such and both cases fall away.
    //
    // The value still applies live, still rides /api/state, and is still writable over HTTP: only
    // the flash write and the dirty mark are suppressed. Set via ControlList::setLive().
    bool live = false;
    // What this surface control drives ("Drivers.brightness"), or null. ONE field for all three
    // kinds: a switch, an encoder and a fader each drive exactly one thing, and three fields would
    // be three ways to say it with two always null.
    const char* surfaceTarget = nullptr;
    // Optional per-control input validator (Text/Password only; nullptr = accept anything
    // that fits the buffer). applyControlValue calls it on the incoming string BEFORE the
    // write and returns ApplyResult::Malformed on reject, so the check covers EVERY write
    // path — HTTP /api/control, APPLY_OP over serial, persistence load — in one place.
    // A control with a wire-format constraint (e.g. deviceModel's printable-ASCII rule)
    // declares it here, so the rule lives with the control, not with any one transport.
    bool (*validate)(const char* value) = nullptr;
};

/// The set of controls a MoonModule exposes to the UI — a module's `controls_`.
///
/// Controls bind to a class variable **by reference** — the descriptor stores a
/// pointer, hot-path code reads the variable directly (zero overhead, no
/// getter/setter). The value lives in the class variable (1–4 bytes); the descriptor
/// is just the metadata UI rendering and persistence need.
///
/// **Memory footprint:** the descriptor stays lean — a variable pointer, a flash
/// name pointer, an `aux` word (Progress total / Select options), the type enum, the
/// int32 min/max, two UI flag bytes, and an optional validate hook (~48 bytes on a
/// 64-bit host, less on ESP32's 32-bit pointers). Descriptors live in a fixed-capacity
/// per-module array — no per-control heap allocation. A module that overflows the
/// default capacity is probably too complex.
///
/// **Persistence and dynamic rebuild:** control values persist via FilesystemModule,
/// which overlays loaded values through each control's pointer during
/// `defineControls()`. Calling `defineControls()` again at runtime (e.g. when a
/// Select changes) clears and rebuilds the set, so only controls relevant to the
/// current mode are shown — this is how conditional `hidden` flags re-evaluate.
///
/// The per-type storage/UI/DMX reference is on `ControlType`; each `addX` method
/// below binds one type.
///
/// **Prior art:** MoonLight's `addControl` binds via
/// `reinterpret_cast<uintptr_t>(&variable)` with UI types
/// "slider"/"select"/"toggle"/"text"/"display"
/// (https://github.com/ewowi/MoonLight/blob/main/src/MoonBase/Nodes.h#L80).
class ControlList {
public:
    ~ControlList() { delete[] controls_; }

    ControlList() = default;
    ControlList(const ControlList&) = delete;
    ControlList& operator=(const ControlList&) = delete;
    ControlList(ControlList&&) = delete;
    ControlList& operator=(ControlList&&) = delete;

    /// Bind a member as a control. ONE NAME for every numeric and boolean type: the widget
    /// follows the variable's own type, which the compiler already knows, so a call cannot
    /// disagree with the declaration and a contributor has one spelling to learn.
    ///
    /// This is deliberately the same vocabulary a MoonLive script uses — `addControl("speed",
    /// speed, 0, 99)` reads identically in a script and in a compiled module, which is the point:
    /// someone who has written one can read the other.
    ///
    /// A non-const lvalue reference binds only to its EXACT type, so overload selection is decided
    /// by the variable alone. No conversion is considered, and no two overloads share a type.
    ///
    /// The WIDGET-specific adders keep their own names — addPin, addSelect, addPalette, addText,
    /// addButton and the rest. They name a widget rather than a width, and that intent is not
    /// recoverable from the C++ type: `uint8_t` backs a slider, a dropdown AND a palette picker,
    /// and an `int8_t` that silently became a Pin would register as a claimed GPIO in PinsModule.
    ///
    /// Bind a `uint8_t` as a 0–255 slider (the preferred default control). `min`/`max`
    /// bound the UI drag range and clamp writes server-side.
    void addControl(const char* name, uint8_t& var, uint8_t min = 0, uint8_t max = 255) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Uint8, min, max};
    }

    /// Bind a `uint16_t` as a number input. min/max default to the full type range
    /// (no UI constraint); pass explicit bounds (e.g. `addControl("sampleRate", r,
    /// 8000, 48000)`) for a bounded slider + server-side write clamp.
    ///
    /// The defaults DIFFER per overload, deliberately: each is its own type's full range, so a
    /// call that omits them means "no UI constraint" whatever the type. Unifying them would
    /// silently move a control's bounds.
    void addControl(const char* name, uint16_t& var,
                    uint16_t min = 0, uint16_t max = UINT16_MAX) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Uint16, min, max};
    }

    // lengthType (int16_t) — signed wire format so negative values round-trip
    // correctly. min/max default to INT16_MIN/INT16_MAX (no UI constraint) when
    // omitted; pass explicit bounds (e.g. addControl("width", w, 1, 512)) to get a
    // bounded slider in the UI and server-side clamping on write.
    void addControl(const char* name, int16_t& var,
                    int16_t min = INT16_MIN, int16_t max = INT16_MAX) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Int16, min, max};
    }

    /// Bind an `int32_t` where the value does not fit 16 bits. min/max default to the
    /// full type range (no UI constraint); pass explicit bounds for a bounded slider +
    /// server-side write clamp.
    void addControl(const char* name, int32_t& var,
                    int32_t min = INT32_MIN, int32_t max = INT32_MAX) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Int32, min, max};
    }

    /// An `int8_t` is NOT a control type here: it is either a GPIO (addPin, which PinsModule
    /// scans for to collect claimed pins) or telemetry (addReadOnlyInt, which needs a unit).
    /// Deducing one from the type would make any future small signed control register as a
    /// claimed GPIO, so the caller says which. Deleted rather than absent, so the diagnostic
    /// names the two real options instead of listing every unrelated overload.
    void addControl(const char* name, int8_t& var, int16_t min = 0, int16_t max = 0) = delete;

    // A GPIO pin number (int8_t storage — one byte; -1 = unused/default). A GPIO
    // never exceeds ~54 on any ESP32-family chip, so int8 (−128..127) is ample and
    // smaller than int16. Renders as a plain number input, not a slider (see
    // ControlType::Pin): a GPIO has no meaningful range to drag. min/max are the
    // valid-GPIO span, used only as a server-side write-clamp guard; the UI keys
    // rendering off the "pin" type string, not the range. The default max is the
    // chip's real GPIO ceiling: MM_MAX_GPIO, which the build defines per-target from
    // the IDF's CONFIG_SOC_GPIO_PIN_COUNT (61 on the S31, whose audio pins reach
    // GPIO57) — derived, not hand-maintained. The fallback below keeps Control.h
    // core/standalone (it compiles with no build flag); the real per-chip value is
    // injected by CMake. Callers don't repeat it.
    void addPin(const char* name, int8_t& var, int16_t min = -1, int16_t max = MM_MAX_GPIO) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Pin, min, max};
    }

    /// A toggle. No min/max: a bool's range is itself, which is why this overload takes three
    /// arguments where the numeric ones take four.
    void addControl(const char* name, bool& var) {
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
        controls_[count_++] = {.ptr = var, .name = name, .type = ControlType::Text,
                               .max = bufSize, .validate = validate};
    }

    // Like addText but the UI renders a resizable multi-line <textarea> (e.g. a
    // script source). Same char-buffer storage and parse/persist behaviour as Text.
    void addTextArea(const char* name, char* var, uint16_t bufSize = 16,
                     bool (*validate)(const char*) = nullptr) {
        grow();
        controls_[count_++] = {.ptr = var, .name = name, .type = ControlType::TextArea,
                               .max = bufSize, .validate = validate};
    }

    // Like addText, but the value NAMES A FILE and the UI edits that file's contents in place.
    // `pick` is a {directory, extension, template} triple the module owns (or nullptr for "no
    // picker"). Extension may be null to list every file; template may be null for "start empty",
    // and is what a newly created file is seeded with, so a new file is a working example rather
    // than a blank that fails to parse. Borrowed, not copied, exactly as addSelect borrows its
    // options array, so a control costs no storage beyond the descriptor.
    //
    // `pick` is typed as an array of EXACTLY THREE, not a bare `const char* const*`: the reader
    // (writeControlMetadata) indexes all three slots, and the loose pointer form accepted a shorter
    // array and read past its end. ASan caught exactly that from a two-element caller. A
    // fixed-length parameter refuses it at compile time, which is where a fixed-size contract
    // belongs; a caller with nothing to offer passes nothing (the no-picker overload below).
    void addFilePath(const char* name, char* var, uint16_t bufSize,
                     const FilePathPick& pick,
                     bool (*validate)(const char*) = nullptr) {
        grow();
        controls_[count_++] = {.ptr = var, .name = name,
                               .aux = reinterpret_cast<uintptr_t>(&pick[0]),
                               .type = ControlType::FilePath,
                               .max = bufSize, .validate = validate};
    }

    /// A file-path control with no picker: an editor over one fixed path, so there is no directory
    /// to list and nothing to seed a new file with.
    void addFilePath(const char* name, char* var, uint16_t bufSize,
                     bool (*validate)(const char*) = nullptr) {
        grow();
        controls_[count_++] = {.ptr = var, .name = name, .aux = 0,
                               .type = ControlType::FilePath,
                               .max = bufSize, .validate = validate};
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

    // A color-palette dropdown: like a Select (ptr → uint8_t index, max = optionCount), but the
    // options carry swatch colors. `optionsFn` (light-domain) emits the {name,colors} objects.
    void addPalette(const char* name, uint8_t& var, PaletteOptionsFn optionsFn, uint8_t optionCount) {
        grow();
        controls_[count_++] = {&var, name, reinterpret_cast<uintptr_t>(optionsFn), ControlType::Palette, 0, optionCount};
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
    // POSTs through to the module's onControlChanged(name), which performs the action. Use
    // for "do this now" (rescan, reset, self-test); use addControl on a bool for on/off state.
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

    // Record what a previously-added control was born with, so the UI can offer a reset for a
    // control whose default cannot be probed from the module TYPE. Used by the scripted modules,
    // whose controls are declared by the running script rather than by the C++ type.
    void setDefault(uint8_t i, int32_t def) {
        if (i < count_) controls_[i].def = def;
    }

    // Flip the readonly flag on a previously-added control. Typical use: call addText()
    // then setReadOnly(count() - 1, true) for a value that's persisted via the standard
    // path but pushed by tooling rather than user-edited (e.g. SystemModule.deviceModel).
    // The UI renders the control display-only; HTTP /api/control writes still apply.
    void setReadOnly(uint8_t i, bool readonly) {
        if (i < count_) controls_[i].readonly = readonly;
    }

    // Flip the "expert only" flag on a previously-added control. Typical use: call addX() then
    // setAdvanced(count() - 1) for a dev/tuning readout or knob (e.g. MoonLed.ringDbg). The UI shows
    // it only when System.expertMode is on; it still persists and still accepts HTTP writes.
    void setAdvanced(uint8_t i, bool advanced = true) {
        if (i < count_) controls_[i].advanced = advanced;
    }

    // Ask the UI to render a numeric control as a plain number input, never a slider — for a value where
    // each integer is a discrete identity (a PHY/I2C address, a channel), not a magnitude to sweep.
    // Typical use: addControl() then setNumberField(count() - 1). See the descriptor's field.
    void setNumberField(uint8_t i, bool numberField = true) {
        if (i < count_) controls_[i].numberField = numberField;
    }

    // Persist this Select by option LABEL (see Control::persistLabel). Typical use: call
    // addSelect() over enumerated options, then setPersistLabel(count() - 1).
    void setPersistLabel(uint8_t i, bool persistLabel = true) {
        if (i < count_) controls_[i].persistLabel = persistLabel;
    }

    /// Render this numeric control as a VERTICAL fader rather than a horizontal slider. Presentation
    /// only: the control, its range and its value are unchanged, and consecutive faders render as
    /// one bank. For a value a user rides rather than sets once — a level — a fader is the physical
    /// affordance, and it is what a hardware surface will map onto.
    /// `target` names what the fader drives ("Drivers.brightness"), or null when nothing yet. A
    /// borrowed pointer, like every other name here.
    void setFader(uint8_t i, bool fader = true, const char* target = nullptr) {
        if (i < count_) { controls_[i].fader = fader; controls_[i].surfaceTarget = target; }
    }

    /// Render this numeric control as a ROTARY ENCODER: a knob, dragged vertically to turn. The
    /// third surface affordance beside pads and faders, and the one both the X-Touch and the QCon
    /// put above their strips. Presentation only, same as setFader — value, range and persistence
    /// are untouched. `target` names what it drives, or null when nothing yet.
    void setEncoder(uint8_t i, bool encoder = true, const char* target = nullptr) {
        if (i < count_) { controls_[i].encoder = encoder; controls_[i].surfaceTarget = target; }
    }

    /// Render this boolean control in the horizontal SWITCH STRIP: a desk's channel buttons, sitting
    /// in the same columns as the encoders and faders below them. Presentation only, like setFader
    /// and setEncoder. Without it eight switches stack as eight ordinary rows, which is both tall
    /// and unreadable as a surface: the point of a strip is that column N is one channel.
    void setSwitchRow(uint8_t i, bool switchRow = true, const char* target = nullptr) {
        if (i < count_) { controls_[i].switchRow = switchRow; controls_[i].surfaceTarget = target; }
    }

    /// Render this read-only text control as the surface's DISPLAY STRIP: a full-width alphanumeric
    /// readout, no label, in the segmented style the numeric readouts already use. Presentation
    /// only, like setFader and setEncoder.
    ///
    /// Nameless and full width because it mirrors the scribble strip above a desk's channels: it
    /// shows whatever was last touched, so a label naming one thing would be wrong as soon as
    /// something else moved, and a narrow cell would truncate the names it exists to show.
    void setDisplayStrip(uint8_t i, bool strip = true) {
        if (i < count_) controls_[i].displayStrip = strip;
    }

    /// Declare a control as LIVE STATE rather than configuration: never persisted, never marks its
    /// module dirty. For a value driven continuously by something other than a person. See
    /// ControlDescriptor::live for why this is a persistence property and not a rendering hint.
    void setLive(uint8_t i, bool live = true) {
        if (i < count_) controls_[i].live = live;
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
// would just get overwritten on the next tick1s).
bool isPersistable(ControlType t);
/// Whether THIS control's value is written to flash. Prefer this over the type-only form: it also
/// honours a ListSource that derives its rows (see ListSource::persistsList).
bool isPersistable(const ControlDescriptor& c);

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
    Ok,            ///< value parsed and applied.
    OutOfRange,    ///< numeric value outside the descriptor's bounds (Strict only).
    Malformed,     ///< the value didn't parse (e.g. a bad IPv4 string).
    ReadOnly,      ///< tried to write a display-only control.
};

// Out-of-range policy for numeric / Select writes. The HTTP API wants
// strict rejection (a bogus client value should surface as a 400 rather
// than silently get clamped); persistence load wants tolerant clamping
// (a stale on-disk value from a schema change should still come close,
// not silently drop to the default-constructed zero).
enum class ApplyPolicy : uint8_t {
    Strict,   ///< reject an out-of-range value (the HTTP API — surfaces as a 400).
    Clamp,    ///< clamp to the nearest valid value (persistence load — tolerates stale on-disk values).
};

// Parse the JSON value at `json[key]` and apply it to the control's storage.
// `json` is the enclosing JSON object's text; the function calls into
// mm::json::parseInt / parseBool / parseString internally to extract the
// right shape per ControlType. Non-Ok results leave the storage untouched.
ApplyResult applyControlValue(const ControlDescriptor& c,
                              const char* json, const char* key,
                              ApplyPolicy policy = ApplyPolicy::Strict);

} // namespace mm
