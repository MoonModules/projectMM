#pragma once

#include <cstdint>
#include <cstddef>
#include "core/moonlive/moonlive_emit.h"

// MoonLive — the live-script engine core (domain-neutral, §3.1/§3.9 of
// livescripts-analysis-top-down.md). Stage 1a: no language yet — compile() asks the per-ISA
// emitter for one fixed routine (fill every light a colour), places it in executable memory,
// and run() calls it over a host-supplied buffer. The whole compiler (lexer → parser → IR →
// codegen) grows in behind compile() over later stages; this is the load-bearing first slice
// that proves emit → allocExec → call → write-the-buffer works on real Xtensa.
//
// Neutral by construction: the engine includes only <cstdint>, the emitter seam, and the
// platform seam — never EffectBase, Buffer, or any projectMM type. The binding
// (src/light/moonlive/MoonLiveEffect.h) wraps it as a MoonModule.

namespace mm::moonlive {

class MoonLive {
public:
    ~MoonLive() { free(); }

    // Compile the (Stage-1a fixed) program for a colour: emit the routine, copy it into an
    // exec block, ready it to call. Returns ok(). A failure (no exec memory, emit too big)
    // leaves the engine !ok() with an error() — the caller degrades, never crashes.
    bool compile(uint8_t r, uint8_t g, uint8_t b);

    // Stage 2: compile SOURCE TEXT. The front-end (MoonLiveCompiler) lexes/parses the
    // `fill(r,g,b);` statement and emits the same code compile(r,g,b) would. A parse error
    // leaves the engine !ok() with error() pointing at the diagnostic — the script editor's
    // failure path. This is the real compile path; the typed overloads above are the
    // hand-driven Stage-1 spikes the source path now subsumes for the fill program.
    bool compile(const char* source);

    // Stage 1b: compile the animated routine (colour derived from the per-frame `t`).
    bool compileAnimated();

    bool ok() const { return fn_ != nullptr || anim_ != nullptr; }
    const char* error() const { return error_; }

    // The hot path: run the compiled routine over the host's buffer. `t` is the host's
    // elapsed() ms; a static routine ignores it, an animated one derives its colour from
    // it. No-op if !ok(), so a failed compile renders nothing rather than calling null.
    void run(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t) const {
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
