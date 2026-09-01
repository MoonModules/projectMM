#pragma once

// Header-only (exception to the core-module .h+.cpp rule): a small read-only diagnostic that reads the
// live module tree + the domain-neutral gpioCapability seam — no translation-unit bulk — same shape and
// rationale as TasksModule.h (which likewise reaches the platform for its snapshot).

#include "core/MoonModule.h"
#include "core/Scheduler.h"   // instance()->moduleCount()/module(i) — the roots of the tree to walk
#include "core/JsonSink.h"    // writeListRow emits its row as JSON into the sink
#include "core/Sort.h"        // insertionSort — order the map by GPIO (Device-Manager keying)
#include "platform/platform.h"  // gpioCapability — is a claimed pin a strap / input-only / reserved?
#include "core/PinList.h"        // parsePinList — the domain-neutral "18,19,20" CSV parser (core primitive)

#include <cstdint>
#include <cstdio>
#include <cstring>   // strcmp / strstr — match control names to a role

namespace mm {

/// A core, domain-neutral diagnostic that shows **which module owns each GPIO, for what role, whether
/// that pin is safe for it, and what it's doing right now** — the device's pin ownership map, keyed by
/// physical GPIO the way an OS Device Manager, a Tasmota template, or a GPIOViewer diagram is. It walks
/// the live module tree and collects every claimed pin: each `ControlType::Pin` control (a mic's
/// `sckPin`/`wsPin`/`sdPin`, an Ethernet PHY's `ethMdcGpio`, a driver's `loopbackTxPin`) and each
/// `"pins"` text control (an LED driver's `"18,19,20"` lane CSV). Per claim it adds a **severity** flag
/// (a claim on a reserved/strap/input-only pin, or a double-claim, via `platform::gpioCapability`) and
/// the **live state** (level + drive strength, via `platform::gpioLiveState` — the see-the-wire HAL
/// check). A **disabled** module's pins drop out (the map reflects release-on-disable intent).
///
/// **One nested list: `pins`.** Each row is a claimed GPIO (`gpio`, `owner`, `role`); a GPIO claimed by
/// more than one control shows the first owner in the summary and every claimant in the row detail, so a
/// double-claim is *visible at a glance* — the exact class of bug the GPIO-46 loopback corruption was (an
/// output role driven onto a strap). Surfacing only: this reads the tree, it does not arbitrate. The
/// controls are already the pin registry; this module is a reader over them, holding no state of its own.
///
/// **Inversion vs the central-manager pattern.** MoonLight's `ModuleIO`
/// (https://github.com/MoonModules/MoonLight) is the *central* pin manager — it owns a JSON pin table and
/// assigns GPIOs. projectMM inverts that: **each module owns its own pins** (as its own controls), and this
/// one central module only *observes* and coordinates. So the reusable idea taken from ModuleIO is its
/// per-pin *report* — owner + usage — not its ownership mechanism; here the owner and role are read back
/// out of the live controls, never from a central table. The role column is name-derived (see `roleFor`),
/// projectMM's equivalent of ModuleIO's `usage` enum without a central vocabulary.
///
/// **Fixed System module.** Wired by code as a child of SystemModule in `main.cpp` (like Tasks and I2cScan),
/// not user-added — you don't delete the pin map. Base `Generic` role so no container accepts it as a
/// user-editable child; `markWiredByCode()` exempts it from the persistence trim. Read-only: no editable
/// controls. The list refreshes on `tick1s()` (a periodic sample, never the hot path), so a live pin change
/// shows on the next second with no reboot. The list uses `ControlType::List` + `ListSource`, the read-only
/// data-source/adapter shape (UITableView's data source, Qt's `QAbstractItemModel`) as TasksModule.
class PinsModule : public MoonModule {
public:
    void defineControls() override {
        MoonModule::defineControls();
        controls_.addList("pins", pins_);
    }

    /// Re-walk the tree once a second (off the hot path) to rebuild the claim map. Each claim copies its
    /// owner name + role into its own storage, so the rows serialize safely even if the owning module is
    /// deleted between refreshes — the snapshot holds no pointers into module memory. The next refresh
    /// rebuilds from the live tree, so a deleted module's claim drops within a second.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        pins_.refresh();
    }

private:
    /// The pin ownership map: a ListSource over a fixed snapshot rebuilt on tick1s. A claim is one GPIO
    /// staked by one control; multiple claims on the same GPIO are kept (a conflict must stay visible, not
    /// be merged away). No allocation — a fixed `Claim[kMaxClaims]`, filled or left at count 0.
    struct PinListSource : ListSource {
        // 64 claims covers any realistic board (a P4 has ~55 GPIOs; a fully-loaded tree of drivers + mic +
        // Ethernet claims far fewer). A diagnostic, so overflow just stops adding rather than allocating.
        static constexpr uint8_t kMaxClaims = 64;
        struct Claim {
            uint8_t gpio;         // the physical GPIO number (row key)
            char owner[16];       // owning module's name(), COPIED in — not a pointer into module storage,
                                  // which a delete frees between refreshes (the UI serializes state right
                                  // after a delete op, so a borrowed pointer would be a live use-after-free,
                                  // not a rare race). 16 matches MoonModule's name_[16] cap.
            char role[14];        // the role, copied in (own storage): a name-derived label ("BCLK",
                                  // "LED lane 0") or the control name. Owned per-claim, not a shared
                                  // buffer, so N lane claims don't collapse onto one string. 14 = the
                                  // worst case "LED lane 255"+NUL (laneIdx is a uint8) and "loopback Tx".
            const char* severity; // "error" (claim on a reserved flash/PSRAM pin), "warn" (a driven role
                                  // on a strap / input-only pin), or nullptr (safe). A static literal, so
                                  // a plain pointer is fine — no per-claim copy needed like owner/role.
            const char* reason;   // WHY it's flagged, shown when the row is expanded ("boot strap",
                                  // "reserved (flash/PSRAM/USB)", "invalid GPIO", "input-only pin",
                                  // "claimed by 2+ controls"), or nullptr when safe. Static literal.
            platform::GpioLiveState live;  // sampled live direction + level + drive strength (the
                                  // see-the-wire axis), read once per refresh; when !live.valid the
                                  // row omits the columns.
        };
        Claim claims_[kMaxClaims];
        uint8_t count_ = 0;

        void refresh() {
            count_ = 0;
            Scheduler* s = Scheduler::instance();
            const uint8_t mc = s ? s->moduleCount() : 0;
            for (uint8_t i = 0; i < mc; i++)
                collect(s->module(i));
            // Order by GPIO so the map reads like a Device Manager (ascending pin), stable for the eye and
            // for drift-tracked scenario snapshots. Insertion sort: tiny n, no allocation.
            insertionSort(claims_, count_, [](const Claim& a, const Claim& b) { return a.gpio < b.gpio; });
            flagConflicts();
        }

        // Soft-flag a GPIO claimed by two or more controls (a conflict — a mic wsPin colliding with an
        // LED lane, two lanes on one pin): after the sort, equal-GPIO claims are adjacent, so one linear
        // pass marks every member of a run of length >= 2 as "error". A conflict is at least as severe as
        // a strap warn, and never downgrades an existing reserved/invalid "error" — so it's simply the max
        // (conflict => "error"). Surface only: the claim still landed (the device keeps running, a live
        // pin-swap isn't wedged) — the map just makes it loud. The row detail already lists the co-owners,
        // so a user seeing the red row expands it and reads exactly which two modules collide.
        void flagConflicts() {
            uint8_t i = 0;
            while (i < count_) {
                uint8_t j = i + 1;
                while (j < count_ && claims_[j].gpio == claims_[i].gpio) j++;
                if (j - i >= 2)   // a run of >= 2 claims on the same GPIO
                    for (uint8_t k = i; k < j; k++) {
                        claims_[k].severity = "error";
                        claims_[k].reason = "claimed by 2+ controls — only one can drive this pin";
                    }
                i = j;
            }
        }

        uint8_t listRowCount() const override { return count_; }

        void writeListRow(JsonSink& sink, uint8_t row) const override {
            if (row >= count_) { sink.append("{}"); return; }
            const Claim& c = claims_[row];
            sink.appendf("{\"gpio\":%u,\"owner\":", static_cast<unsigned>(c.gpio));
            sink.writeJsonString(c.owner);
            sink.append(",\"role\":");
            sink.writeJsonString(c.role);
            // Emit `severity` only when the pin is unsafe — the UI keys its generic per-row warning
            // color on this field (like the `*Sec` age convention); a safe pin omits it, no color.
            if (c.severity) {
                sink.append(",\"severity\":");
                sink.writeJsonString(c.severity);
            }
            // Live-state columns (the see-the-wire axis) — omitted entirely when the pad isn't readable
            // (desktop / invalid pin), so the row simply has no live fields there. `dir` is the pin's
            // ACTUAL live direction off the pad (out / in / both / off), not the role's intent.
            if (c.live.valid) {
                sink.append(",\"dir\":");
                sink.writeJsonString(dirLabel(c.live));
                sink.append(",\"level\":");
                sink.writeJsonString(c.live.level ? "HIGH" : "LOW");
                sink.append(",\"drive\":");
                sink.writeJsonString(driveLabel(c.live.driveCap));
            }
            sink.append("}");
        }

        // The IDF drive-capability enum (0..3) as a bench-readable label — GPIO_DRIVE_CAP_0..3 map to
        // ~5 / 10 / 20 / 40 mA on the ESP32. A driven pin under-driving a long strip or a level shifter
        // shows here (the signal-integrity check §6 names).
        static const char* driveLabel(uint8_t cap) {
            switch (cap) {
                case 0:  return "WEAK";
                case 1:  return "MEDIUM";
                case 2:  return "STRONG";
                default: return "STRONGEST";
            }
        }

        // The pin's ACTUAL live direction off the pad config (output/input enable), not the role's
        // intent — "both" is normal for an open-drain bus (I²C) or a bidirectional line; "off" means
        // neither buffer is enabled (the pin is idle / not yet configured).
        static const char* dirLabel(const platform::GpioLiveState& live) {
            if (live.output && live.input) return "both";
            if (live.output) return "out";
            if (live.input)  return "in";
            return "off";
        }

        // Row detail = every claim on this row's GPIO (as "owner · role" chips — a double-claim lists all
        // co-owners), plus a `warning` line explaining WHY the row is flagged when it is (boot strap,
        // reserved, input-only, conflict — so the colored edge isn't just "something's wrong" but names
        // it). Scalar fields, so the generic list-detail UI renders the chips + the warning line with no
        // pins-specific UI code.
        void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
            if (row >= count_) { sink.append("{}"); return; }
            const uint8_t gpio = claims_[row].gpio;
            sink.append("{\"claims\":[");
            bool first = true;
            const char* reason = nullptr;
            for (uint8_t i = 0; i < count_; i++) {
                if (claims_[i].gpio != gpio) continue;
                if (!first) sink.append(",");
                first = false;
                char line[80];
                std::snprintf(line, sizeof(line), "%s \xC2\xB7 %s", claims_[i].owner, claims_[i].role);
                sink.writeJsonString(line);
                if (claims_[i].reason) reason = claims_[i].reason;   // the flag reason for this GPIO
            }
            sink.append("]");
            if (reason) {
                sink.append(",\"warning\":");
                sink.writeJsonString(reason);
            }
            sink.append("}");
        }

    private:
        // Recurse a module + its children, recording every claimed pin. Depth-first so children's pins list
        // under the whole tree, not just the roots.
        void collect(MoonModule* m) {
            if (!m) return;
            // A DISABLED module has stopped acting, so it no longer holds its pins as far as the map is
            // concerned — skip its claims. Uses effectivelyEnabled() (not the raw enabled() flag) so a
            // module under a DISABLED PARENT also shows its pins freed: the applyState() cascade tears
            // that module's peripheral down (a disabled parent releases its whole subtree), so the map
            // must match — a child of a disabled Drivers/Layer container frees its GPIOs too. The gate
            // is exactly the router's: applyState() calls release() on any node that is NOT
            // effectivelyEnabled(), so the map's "active" must be the same predicate or it would show a
            // pin held that the router already freed. The map's "freed" is truthful: release() deinits
            // the peripheral (RMT/Parlio/LCD/I²S/socket/IR), so the GPIO is genuinely reusable — no reboot.
            const bool active = m->effectivelyEnabled();
            const ControlList& cl = m->controls();
            for (uint8_t i = 0; active && i < cl.count(); i++) {
                const ControlDescriptor& d = cl[i];
                // A HIDDEN pin control means the peripheral isn't using that GPIO right now — the
                // control still exists (its value/struct layout is unconditional for persistence),
                // but a conditional `setHidden` gated it off: the W5500 SPI pins when ethType != SPI,
                // the loopback Tx/Rx pins when loopbackTest is off. Its pin is genuinely free, so it
                // must NOT count as a claim, or e.g. setting Ethernet to None would still show its
                // MISO pin conflicting with a real user (an IR receiver on the same GPIO). Skip it.
                if (d.hidden) continue;
                if (d.type == ControlType::Pin) {
                    const int8_t v = *static_cast<int8_t*>(d.ptr);
                    if (v >= 0) addPinClaim(static_cast<uint8_t>(v), m->name(), roleFor(d.name));
                } else if (d.type == ControlType::Text && isPinListName(d.name)) {
                    // A pin CSV ("18,19,20"): one claim per pin. Matched on the NAME ENDING in "pins"
                    // rather than being exactly "pins", so a module with more than one list is
                    // visible here too (Drivers' "relayPins" beside an LED driver's "pins"); a claim
                    // the map cannot see is a pin nothing guards. parsePinList dedups within the one
                    // string, so a list never self-collides, and it rejects any out-of-range pin
                    // (> MM_MAX_GPIO) outright, so every returned pin is a real GPIO.
                    uint16_t pins[kMaxClaims];
                    uint8_t n = 0;
                    if (!parsePinList(static_cast<const char*>(d.ptr), pins, kMaxClaims, n))
                        for (uint8_t p = 0; p < n; p++)
                            addLaneClaim(static_cast<uint8_t>(pins[p]), m->name(), d.name, p);
                }
            }
            // Pins the module holds that are not controls: silicon-fixed pads (an EMAC's data bus).
            // Asked of the module rather than listed here, because which pads and whether they are
            // held at all are the module's own facts. Gated on `active` like every other claim, so a
            // disabled interface frees them.
            if (active) {
                MoonModule::FixedPin fixed[16];
                const uint8_t n = m->fixedPins(fixed, 16);
                for (uint8_t i = 0; i < n; i++) addPinClaim(fixed[i].gpio, m->name(), fixed[i].role);
            }
            // Always recurse children regardless of this module's flag — a child is judged on its own
            // enabled state, not its parent's.
            for (uint8_t i = 0; i < m->childCount(); i++)
                collect(m->child(i));
        }

        // The claim-adders copy BOTH owner and role into the claim's own buffers, so no claim holds a
        // pointer into module or shared storage — the snapshot survives a module delete between refreshes.
        Claim* reserve(uint8_t gpio, const char* owner) {
            if (count_ >= kMaxClaims) return nullptr;   // diagnostic: drop past the cap, never allocate
            Claim& c = claims_[count_++];
            c.gpio = gpio;
            c.severity = nullptr;
            c.reason = nullptr;
            c.live = platform::gpioLiveState(gpio);   // sample the live pad now (tick1s, off the hot path)
            std::snprintf(c.owner, sizeof(c.owner), "%s", owner ? owner : "");   // always NUL-terminates
            return &c;
        }
        void addPinClaim(uint8_t gpio, const char* owner, const char* role) {
            if (Claim* c = reserve(gpio, owner)) {
                std::strncpy(c->role, role, sizeof(c->role) - 1);
                c->role[sizeof(c->role) - 1] = '\0';
                gradeClaim(*c, isOutputRole(role));
            }
        }
        /// A control name that holds a comma-separated pin list: "pins", or anything ending in it
        /// ("relayPins"). One rule rather than a list of names, so a new list is seen by the map the
        /// moment it follows the convention.
        static bool isPinListName(const char* name) {
            const size_t n = std::strlen(name);
            if (n < 4) return false;
            // Case-insensitive on the suffix, because the convention is camelCase: an LED driver's
            // control is "pins" and Drivers' is "relayPins", and a case-sensitive compare silently
            // matched only the first. Silently, because a missed claim looks exactly like a pin
            // nobody uses.
            const char* s = name + n - 4;
            return (s[0] == 'p' || s[0] == 'P') && s[1] == 'i' && s[2] == 'n' && s[3] == 's';
        }

        void addLaneClaim(uint8_t gpio, const char* owner, const char* control, uint8_t laneIdx) {
            if (Claim* c = reserve(gpio, owner)) {
                // Named after the control, so a row says what the pin IS: an LED driver's "pins"
                // reads "LED lane 0", Drivers' "relayPins" reads "relay 0".
                const bool isLed = std::strcmp(control, "pins") == 0;
                std::snprintf(c->role, sizeof(c->role), isLed ? "LED lane %u" : "relay %u",
                              static_cast<unsigned>(laneIdx));
                gradeClaim(*c, /*isOutput=*/true);   // both drive the pin
            }
        }

        // Grade a claim against the pin's hardware capability (platform::gpioCapability), setting BOTH
        // its severity (the row color) and its reason (shown when the row is expanded — the *why*). In
        // priority order: reserved (flash/PSRAM/USB) or invalid GPIO → "error" for ANY role (routing I/O
        // there corrupts the device / isn't a pin); a driven role on an input-only pin or a boot strap →
        // "warn" (the GPIO-46-strap class of bug). An input role on an input-only pin is fine (no flag).
        // The conflict case (a GPIO claimed twice) is graded separately in flagConflicts, after the sort.
        //
        // Note: the pin's LIVE direction (c.live, shown as the `dir` column) is deliberately NOT graded.
        // "role drives but the pad reads input/off" looked like a dead-driver check, but too many pins are
        // legitimately not-driving-when-idle — I²C is bidirectional (idle = input), a loopback Tx is off
        // until the self-test runs, an RMII clock can be an external input. Auto-flagging those is noise
        // that erodes trust in the red/orange edges, so `dir` is shown as information the operator reads,
        // not a warning.
        static void gradeClaim(Claim& c, bool isOutput) {
            const platform::GpioCapability cap = platform::gpioCapability(c.gpio);
            if (!cap.validGpio)      { c.severity = "error"; c.reason = "invalid GPIO — not a pin on this chip"; return; }
            if (cap.reserved)        { c.severity = "error"; c.reason = "reserved for flash / PSRAM / USB"; return; }
            if (isOutput && !cap.outputCapable) { c.severity = "warn"; c.reason = "input-only pin — a driven role can't output here"; return; }
            if (isOutput && cap.strap)          { c.severity = "warn"; c.reason = "boot strap — driving it at reset can change boot mode"; return; }
        }

        // Does this role drive the pin (output) vs. read it (input)? Name-derived, same spirit as
        // roleFor: LED lanes, a loopback Tx, and the Ethernet clock/reset/MDC/MDIO lines are driven;
        // a mic's data/clock inputs (sd/sck/ws) and a loopback Rx are read. Defaults to output (the
        // conservative choice — a mislabelled driven pin on a strap is the bug we most want flagged).
        static bool isOutputRole(const char* role) {
            // The clearly-input roles: mic lines + the loopback receive pin. Everything else is treated
            // as driven, so an unknown output-ish control errs toward showing the warning.
            return !(std::strcmp(role, "data") == 0 || std::strcmp(role, "BCLK") == 0 ||
                     std::strcmp(role, "WS") == 0   || std::strcmp(role, "MCLK") == 0 ||
                     std::strcmp(role, "loopback Rx") == 0);
        }

        // Name → role: projectMM's name-derived equivalent of ModuleIO's usage enum. A small display
        // convenience, not a vocabulary authority — an unmatched name falls through to itself, which is
        // still informative. Returns a static literal (or the control name, which lives in static/module
        // storage); the caller copies it into the claim, so the returned pointer needn't outlive the call.
        static const char* roleFor(const char* name) {
            struct Entry { const char* suffix; const char* role; };
            static constexpr Entry kRoles[] = {
                {"sckPin",        "BCLK"},        {"wsPin",         "WS"},
                {"sdPin",         "data"},        {"mclkPin",       "MCLK"},
                {"loopbackTxPin", "loopback Tx"}, {"loopbackRxPin", "loopback Rx"},
                {"ethMdcGpio",    "MDC"},         {"ethMdioGpio",   "MDIO"},
                {"sda",           "I\xC2\xB2""C SDA"}, {"scl",       "I\xC2\xB2""C SCL"},
            };
            for (const Entry& e : kRoles)
                if (std::strcmp(name, e.suffix) == 0) return e.role;
            return name;   // fall through to the control name — still informative
        }
    };

    PinListSource pins_;
};

} // namespace mm
