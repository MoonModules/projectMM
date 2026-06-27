#pragma once

#include <cstdint>
#include <cstddef>
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveBuiltins.h"

// MoonLive — the live-script engine core (domain-neutral, §3.1/§3.9 of
// livescripts-analysis-top-down.md). compile() turns a program into native code — either a
// source string (via MoonLiveCompiler) or a fixed routine direct from the emitter — places it
// in executable memory, and run() calls it over a host-supplied buffer. The path is
// emit → allocExec → call → write-the-buffer, running on Xtensa, RISC-V, and the desktop host.
//
// Neutral by construction: the engine includes only <cstdint>, the compiler/emitter seams, and
// the platform seam — never EffectBase, Buffer, or any projectMM type. The binding
// (src/light/moonlive/MoonLiveEffect.h) wraps it as a MoonModule.

namespace mm::moonlive {

class MoonLive {
public:
    MoonLive() = default;
    ~MoonLive() { free(); }

    // Owns a heap-backed exec block (freed in the destructor), so copying would duplicate
    // ownership and double-free. Non-copyable; each scripted module holds its own engine.
    MoonLive(const MoonLive&) = delete;
    MoonLive& operator=(const MoonLive&) = delete;

    // Compile a fixed-colour program direct from the emitter: emit the routine, copy it into
    // an exec block, ready it to call. Returns ok(). A failure (no exec memory, emit too big)
    // leaves the engine !ok() with an error() — the caller degrades, never crashes.
    bool compile(uint8_t r, uint8_t g, uint8_t b);

    // Compile SOURCE TEXT, resolving function calls against `table` (the host's registered
    // built-ins — see MoonLiveBuiltins.h). The front-end parses an expression-call statement
    // and lowers it through the IR + per-ISA assembler. A parse/codegen error leaves the engine
    // !ok() with error() pointing at the diagnostic — the script editor's failure path.
    bool compile(const char* source, const BuiltinTable& table);

    // Compile the animated routine (colour derived from the per-frame `t`).
    bool compileAnimated();

    bool ok() const { return fn_ != nullptr || anim_ != nullptr; }
    const char* error() const { return error_; }

    // The hot path: run the compiled routine over the host's buffer. `t` is the host's
    // elapsed() ms; a static routine ignores it, an animated one derives its colour from
    // it. No-op if !ok() (a failed compile renders nothing). The emitted routines write
    // channels +0/+1/+2 per light, so a buffer that can't hold RGB — null, zero lights, or
    // fewer than 3 channels per light — is left untouched rather than overrun (robust to any
    // grid size / layout, the hard rule).
    void run(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t) const {
        if (!buf || nLights == 0 || cpl < 3) return;
        if (fn_) fn_(buf, nLights, cpl);
        else if (anim_) anim_(buf, nLights, cpl, t);
    }

    // Release the exec block (the "destructor" role — teardown returns the memory).
    void free();

    // The emitted code length, for the golden-bytes test (0 until compiled).
    size_t codeLen() const { return codeLen_; }

private:
    // Shared post-emit step: copy `len` staged bytes into a fresh exec block. Returns the
    // block (already writeExec'd) or nullptr on failure (error_ set). The typed pointer is
    // cast by the caller.
    void* place(const uint8_t* staged, size_t len);

    void*   code_ = nullptr;     // allocExec block holding the emitted machine code
    size_t  codeCap_ = 0;        // its capacity (for freeExec)
    size_t  codeLen_ = 0;        // bytes actually emitted
    FillFn  fn_ = nullptr;       // static fill (3-arg), or nullptr
    AnimFn  anim_ = nullptr;     // animated fill (4-arg, reads t), or nullptr
    const char* error_ = "";
};

}  // namespace mm::moonlive
