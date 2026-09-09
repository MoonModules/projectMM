#pragma once

#include <cstdint>
#include <cstddef>
#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/moonlive/MoonLiveIr.h"      // DeclaredControl, kMaxCtrls (surfaced on CompileResult)
#include "core/moonlive/moonlive_emit.h"   // RegBudget — the test-only register-budget override

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

/// The diagnostic when lowering produces nothing: no backend for this ISA, or a program too large.
/// Named so a test can distinguish "this host has no JIT" from "this script is wrong" without
/// matching on prose.
inline constexpr const char* kCodegenFailed = "codegen failed (unsupported on this target, or too large)";
inline constexpr const char* kSpillRefused  = "codegen failed: too many live values for this chip's registers";


// Result of compiling source: on success, ok==true and the bytes are in out[0..len). On
// failure, ok==false and error points at a static diagnostic (1-based column, 0 if n/a).
/// A function the script defined, and where its code starts within the emitted block.
///
/// This is a symbol table, which is what every compiler and linker keeps: one code section, and a
/// name-to-offset map over it. The binding asks for an entry by name and gets a callable address,
/// so which ROLE a script plays is decided by which entries it defined rather than by its type.
/// What a function hands back. `void` is the default because it is what every entry point that
/// ACTS rather than answers declares, and the three cases are all the language has values for.
///
/// `Str` is a literal's pointer, not a string type: a script returns one, it cannot build,
/// concatenate or compare one. Naming the type says what comes back without implying the rest.
enum class RetType : uint8_t { Void, Int, Str };

struct EntryPoint {
    const char* name = nullptr;    ///< into the source, or the engine's own copy after compile
    uint8_t     nameLen = 0;
    uint16_t    offset = 0;        ///< byte offset of its first instruction within the block
    /// What this function returns, as the script DECLARED it. The host reads a value only from a
    /// function that says it has one: calling a `void` function through ValueFn reads whatever sat
    /// in the return register, which is a plausible number rather than an obvious failure.
    RetType     ret = RetType::Void;
};

/// How many named functions one script may define. A handful of entry points plus the helpers a
/// script writes for itself; past that is a script that wants to be a module.
inline constexpr uint8_t kMaxEntryPoints = 8;

/// Longest function name the engine keeps. The host's own entry names (`placeLights`,
/// `modifyLogicalSize`) are the long ones; a script's helpers are usually short.
inline constexpr uint8_t kMaxEntryName = 23;

/// Longest class name kept. A diagnostic quotes it, so it is bounded like every other name the
/// engine holds rather than pointing into source that is freed as soon as the compile returns.
inline constexpr size_t kMaxClassName = 31;

struct CompileResult {
    bool        ok = false;
    const char* error = "";
    uint16_t    errorCol = 0;
    size_t      len = 0;
    // Every member the class declared (`uint8_t speed = 50;`). The engine seeds each one's arena
    // byte with its initializer, which is what "a member is initialized once" means: a member the
    // UI never shows still has to start at the value the script wrote.
    DeclaredControl members[kMaxCtrls];
    uint8_t         memberCount = 0;
    // Text a script wrote as a string literal, NUL-separated. The source buffer is freed as soon
    // as a compile returns, so a `const char*` the emitted code carries cannot point into it. The
    // parser interns each literal directly into this pool, which the ENGINE owns and outlives the
    // compile, so the address the emitted code carries is already its final one: nothing is copied
    // or rebased afterwards. 128 bytes is a handful of control labels, all a string is used for.
    static constexpr uint16_t kStringPool = 128;
    // How many of those bytes this program interned, so a binding can report the headroom.
    uint16_t        stringLen = 0;
    // The name the script gave its class. What diagnostics and the module status report, so a
    // renamed FILE does not change what a user is told: the filename is what the engine loads, the
    // class name is what it is. Copied out of the source, which is freed after the compile.
    char            className[kMaxClassName + 1] = "";
    // The functions this script defined, in source order. `tick` is the one the light bindings look
    // for today; the rest arrive with the per-role entry points.
    EntryPoint      entries[kMaxEntryPoints];
    uint8_t         entryCount = 0;
};

// Compile `source` to machine code in `out` (capacity `cap`), resolving calls against `table`.
// Pure: no I/O, no allocation beyond the caller's buffer.
/// `sysvars` are names the HOST defines and a script may only read (`t`, `width`, …). They are
/// reserved: a declaration that reuses one fails to compile, so a name means one thing everywhere.
/// `squeeze` overrides the register budget the backend would pick for itself. It is the seam that
/// makes the register allocator testable: on a host with fourteen registers, compiling the same
/// script at the full budget and at a deliberately smaller one must render IDENTICAL pixels — the
/// spilled program and the unspilled one are the same program. Production callers omit it.
/// The IR→bytes step, as a function pointer. Normally null, meaning "this build's own backend".
/// A test passes ANOTHER ISA's lowerer to read what a device would execute without flashing one —
/// the front end is identical either way, so the seam is one pointer rather than a second compiler.
using LowerFn = size_t (*)(IrProgram&, uint8_t*, size_t, const RegBudget*);

/// `strings` is where string literals are interned, supplied by the CALLER because the emitted
/// code carries pointers into it: the parser's own storage dies with the compile, so a pointer
/// made there would dangle before the program ran. The engine passes its own member, which lives
/// as long as the compiled program. Null is allowed for a caller with no string literals to
/// support (the codegen tests), and a script that uses one then fails with a diagnostic.
CompileResult compileSource(const char* source, const BuiltinTable& table,
                            const SysVarTable& sysvars, uint8_t* out, size_t cap,
                            const RegBudget* squeeze = nullptr, LowerFn lower = nullptr,
                            char* strings = nullptr, uint16_t stringCap = 0);

/// Tokens in `source`, the one measure both right-sized buffers derive from: the caller sizes its
/// code buffer with `codeCapFor`, and compileSource sizes the IR op array from the same count. One
/// function so a caller cannot measure the script differently from the compiler.
uint32_t countTokens(const char* source);

}  // namespace mm::moonlive
