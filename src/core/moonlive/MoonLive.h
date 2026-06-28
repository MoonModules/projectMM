#pragma once

#include <cstdint>
#include <cstddef>
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/moonlive/MoonLiveCompiler.h"   // CompileResult (carries the declared controls)

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

    bool ok() const { return fn_ != nullptr || anim_ != nullptr || ctrl_ != nullptr; }
    const char* error() const { return error_; }

    // The hot path: run the compiled routine over the host's buffer. `t` is the host's
    // elapsed() ms; a static routine ignores it, an animated one derives its colour from
    // it. No-op if !ok() (a failed compile renders nothing). The emitted routines write
    // channels +0/+1/+2 per light, so a buffer that can't hold RGB — null, zero lights, or
    // fewer than 3 channels per light — is left untouched rather than overrun (robust to any
    // grid size / layout, the hard rule).
    void run(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t) const {
        if (!buf || nLights == 0 || cpl < 3) return;
        if (ctrl_) ctrl_(buf, nLights, cpl, t, ctrlArena_);   // front-end-compiled (reads controls)
        else if (fn_) fn_(buf, nLights, cpl);                 // hand-encoded fixed fill
        else if (anim_) anim_(buf, nLights, cpl, t);          // hand-encoded animated fill
    }

    // Release the exec block + the control arena (the "destructor" role — teardown returns the
    // memory).
    void free();

    // The emitted code length, for the golden-bytes test (0 until compiled).
    size_t codeLen() const { return codeLen_; }
    // The allocated exec-block size (word-rounded codeLen) — the actual heap held, for memory
    // accounting. 0 until compiled / after free().
    size_t codeCap() const { return codeCap_; }

    // The controls the last compile() declared (empty if none / not a source compile). The binding
    // reads this to create real MoonModule controls bound to the arena slots.
    const DeclaredControl* declaredControls(uint8_t& count) const { count = controlCount_; return controls_; }
    // The backing byte for control `offset` in the live arena — the binding points a uint8 control
    // reference here. nullptr if offset is out of range. Stable across a normal recompile (the
    // arena only grows its capacity), so a bound control pointer survives a source edit.
    uint8_t* controlSlot(uint8_t offset) { return (ctrlArena_ && offset < controlCount_) ? &ctrlArena_[offset] : nullptr; }

private:
    // Shared post-emit step: copy `len` staged bytes into a fresh exec block. Returns the
    // block (already writeExec'd) or nullptr on failure (error_ set). The typed pointer is
    // cast by the caller.
    void* place(const uint8_t* staged, size_t len);

    // Drop the prior compilation's code (exec block + fn pointers) but keep the control arena, so a
    // recompile re-emits cleanly without moving the arena a bound control pointer references.
    void freeCode();

    // Ensure the control arena holds `count` bytes, seeding new slots from `decls[i].def`. Grows
    // capacity (never shrinks/moves) so a control pointer the binding holds stays valid across a
    // recompile; preserves an existing slot's live value when the script is edited but the control
    // persists. Returns false on alloc failure (the caller degrades).
    bool ensureArena(const DeclaredControl* decls, uint8_t count);

    void*   code_ = nullptr;     // allocExec block holding the emitted machine code
    size_t  codeCap_ = 0;        // its capacity (for freeExec)
    size_t  codeLen_ = 0;        // bytes actually emitted
    FillFn  fn_ = nullptr;       // static fill (3-arg), or nullptr
    AnimFn  anim_ = nullptr;     // animated fill (4-arg, reads t), or nullptr
    CtrlFn  ctrl_ = nullptr;     // front-end-compiled routine (5-arg, reads the controls arena)
    const char* error_ = "";

    uint8_t* ctrlArena_ = nullptr;   // live control-value bytes (platform::alloc, PSRAM-first)
    uint8_t  ctrlArenaCap_ = 0;      // allocated capacity (grows only)
    uint8_t  controlCount_ = 0;      // controls the current program declared
    DeclaredControl controls_[kMaxCtrls] = {};   // the declared-control metadata for the binding
};

}  // namespace mm::moonlive
