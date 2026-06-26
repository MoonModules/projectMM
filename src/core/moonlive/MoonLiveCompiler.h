#pragma once

#include <cstdint>
#include <cstddef>

// MoonLive front-end (§3.2) — the platform-independent compiler: source text → tokens →
// AST → native code (via the per-ISA emitter). Stage 2 is the smallest real slice: it
// recognises ONE statement,
//
//     fill(<r>, <g>, <b>);
//
// three integer 0..255 args, and emits the SAME machine code the hand-written emitFill
// produces (the golden-bytes equivalence is the no-language-leak proof). No expressions,
// variables, or control flow yet — the lexer/parser/codegen shape is real, the grammar is
// one rule. It grows rule by rule from here; the IR seam is introduced when a second
// statement/type forces it (concrete-first).
//
// Neutral by construction: the compiler knows the LANGUAGE, never an ISA — codegen calls
// the platform's emitFill, so a different backend changes nothing here.

namespace mm::moonlive {

// Result of compiling source: on success, ok==true and the emitted bytes are in out[0..len).
// On failure, ok==false and error points at a static, human-readable diagnostic (the column
// is 1-based, 0 if not applicable). out is filled only on success.
struct CompileResult {
    bool        ok = false;
    const char* error = "";
    uint16_t    errorCol = 0;
    size_t      len = 0;        // bytes written to out on success
};

// Compile `source` into machine code in `out` (capacity `cap`). Pure: no I/O, no allocation
// beyond the caller's buffer — the same input always yields the same bytes, so it unit-tests
// trivially (and the result is asserted byte-for-byte against emitFill). The emitted code is
// the per-ISA fill routine for the parsed colour.
CompileResult compileSource(const char* source, uint8_t* out, size_t cap);

}  // namespace mm::moonlive
