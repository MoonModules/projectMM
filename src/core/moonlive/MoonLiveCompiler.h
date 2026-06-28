#pragma once

#include <cstdint>
#include <cstddef>
#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/moonlive/MoonLiveIr.h"      // DeclaredControl, kMaxCtrls (surfaced on CompileResult)

// MoonLive front-end (§3.2) — the platform-independent compiler: source text → tokens → AST →
// IR → native code (via the per-ISA assembler). The grammar is a single statement that is a
// call to a host-registered function, with EXPRESSION arguments:
//
//     stmt := call ";"
//     call := ident "(" [expr {"," expr}] ")"
//     expr := number | call            // any argument may be a nested call, e.g. random16(256)
//
// Neutral by construction: the compiler knows the LANGUAGE and resolves call names against the
// injected BuiltinTable — it owns no function names and no domain (LED) semantics. setRGB/fill/
// random16 live in the host's table; the compiler emits a generic Call (for Kind::Call) or a
// generic Inline op (for Kind::Inline) and lets the backend lower it.

namespace mm::moonlive {

// Result of compiling source: on success, ok==true and the bytes are in out[0..len). On
// failure, ok==false and error points at a static diagnostic (1-based column, 0 if n/a).
struct CompileResult {
    bool        ok = false;
    const char* error = "";
    uint16_t    errorCol = 0;
    size_t      len = 0;
    // Controls the script declared (`uint8_t speed = 50; // @control 0..99`). The binding reads
    // this list and creates a real MoonModule control per entry, bound to the run-time arena slot.
    DeclaredControl controls[kMaxCtrls];
    uint8_t         controlCount = 0;
};

// Compile `source` to machine code in `out` (capacity `cap`), resolving calls against `table`.
// Pure: no I/O, no allocation beyond the caller's buffer.
CompileResult compileSource(const char* source, const BuiltinTable& table, uint8_t* out, size_t cap);

}  // namespace mm::moonlive
