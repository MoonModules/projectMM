#pragma once

#include <cstdint>
#include <cstddef>

// MoonLive built-in table — the neutral seam by which a HOST registers the functions a script
// may call (the ESPLiveScript `arti_external_function` / ARTI / doc §3.4 model). The core
// compiler knows only *that a name maps to a descriptor*; it owns no function names and no
// domain semantics. The light domain (or any other host) populates the table with its own
// vocabulary — setRGB/fill/random16 for LEDs, something else for a display or a sensor.
//
// A descriptor says how a call lowers:
//   - Call   — a pure host helper: lower to a generic call to `fn` (a C function pointer),
//              one argument in, one result out. (random16, later sin/cos/hsvToRgb…)
//   - Inline — a routine the backend emits inline (no per-call overhead — the hot-path
//              writers): the descriptor carries an `inlineOp` TAG, a neutral opcode the
//              per-ISA lowering knows how to emit. The core never interprets the tag; it just
//              threads it through. The light domain decides which names map to which tags.
//
// This is what keeps the core domain-neutral while the hot path stays inline: the *name*
// "setRGB" and its RGB meaning live only in the host's registration; the core sees a tag.

namespace mm::moonlive {

// Neutral inline opcodes — "store shapes a backend can emit", not "LED operations". A host maps
// its function names onto these; a backend implements them. StoreElem = store N bytes (one
// element) at a computed index; FillElems = a counted loop writing one element per slot. The
// core treats them as opaque tags; the per-ISA backend and the host both know the element is 3
// bytes (RGB) for the light host, but that meaning lives outside the core.
enum class InlineOp : uint8_t {
    StoreElem,   // operands: bufVReg, indexVReg, v0, v1, v2  → store one element at index
    FillElems,   // operands: bufVReg, countVReg, strideVReg, v0, v1, v2  → loop store over count
};

enum class BuiltinKind : uint8_t { Call, Inline };

// A host callable: one unsigned arg in, one unsigned result out (the shape a unary script
// helper like random16 has). A typed alias keeps the table and IR type-safe across desktop and
// ESP32 instead of threading a bare void*.
using HostCallFn = uint32_t (*)(uint32_t);

struct Builtin {
    const char*  name = nullptr;      // the script-visible name (host-owned)
    uint8_t      argc = 0;            // number of arguments
    bool         returns = false;     // Call: produces a value (an expression) vs a void statement
    BuiltinKind  kind = BuiltinKind::Call;
    HostCallFn   fn = nullptr;        // Call: the host C function pointer
    InlineOp     inlineOp{};          // Inline: the neutral opcode tag
};

// A fixed-capacity table the host fills and the compiler reads. No heap; a host registers a
// handful of functions. Lookup is by name (linear — the table is tiny).
struct BuiltinTable {
    static constexpr uint8_t kMax = 16;
    Builtin items[kMax];
    uint8_t count = 0;

    bool add(const Builtin& b) {
        if (count >= kMax || b.name == nullptr) return false;   // a null name would null-deref in find()
        items[count++] = b;
        return true;
    }
    const Builtin* find(const char* name, size_t len) const {
        for (uint8_t i = 0; i < count; i++) {
            const char* n = items[i].name;
            size_t j = 0;
            for (; j < len && n[j]; j++) if (n[j] != name[j]) break;
            if (j == len && n[j] == 0) return &items[i];
        }
        return nullptr;
    }
};

}  // namespace mm::moonlive
