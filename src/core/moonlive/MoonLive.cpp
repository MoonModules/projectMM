#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveCompiler.h"
#include "platform/platform.h"

namespace mm::moonlive {

// Fixed cap for an emitted routine. Sized for the heaviest realistic single statement — a
// setRGB with all four arguments a host call (4 × a full register-save call sequence, ~140
// bytes each on RISC-V, the bulkiest ISA, plus the inline store). The emitter returns the real
// length and the unused tail is harmless; exec memory is cheap, so we size for the worst case
// rather than grow per script. Word-aligned so allocExec/writeExec's word-rounding never
// exceeds it.
static constexpr size_t kCodeCap = 768;

// Copy `len` already-emitted bytes into a fresh exec block. writeExec hides the ISA quirks
// (IRAM's 32-bit-store-only rule, the I-cache sync), so the engine stays target-agnostic.
// Returns the block (ready to call) or nullptr on failure (error_ set, prior state freed).
void* MoonLive::place(const uint8_t* staged, size_t len) {
    free();   // drop any prior compilation — (re)compile is a clean re-emit
    if (len == 0) { error_ = "emit failed"; return nullptr; }
    // Allocate only what was emitted, word-rounded (writeExec stores 32-bit words on IRAM), not
    // the worst-case kCodeCap — a fill is ~50 bytes, a four-call setRGB ~600. The staging buffer
    // is sized for the worst case; the live exec block is sized for THIS program.
    size_t cap = (len + 3) & ~size_t(3);
    void* block = platform::allocExec(cap);
    if (!block) { error_ = "no executable memory"; return nullptr; }
    platform::writeExec(block, staged, len);
    code_ = block;
    codeCap_ = cap;
    codeLen_ = len;
    error_ = "";
    return block;
}

bool MoonLive::compile(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t staging[kCodeCap];
    size_t len = emitFill(staging, kCodeCap, r, g, b);
    void* block = place(staging, len);
    if (!block) return false;
    fn_ = reinterpret_cast<FillFn>(block);
    return true;
}

bool MoonLive::compile(const char* source, const BuiltinTable& table) {
    uint8_t staging[kCodeCap];
    CompileResult cr = compileSource(source, table, staging, kCodeCap);
    if (!cr.ok) { free(); error_ = cr.error; return false; }   // surface the parse diagnostic
    void* block = place(staging, cr.len);
    if (!block) return false;
    fn_ = reinterpret_cast<FillFn>(block);
    return true;
}

bool MoonLive::compileAnimated() {
    uint8_t staging[kCodeCap];
    size_t len = emitAnimatedFill(staging, kCodeCap);
    void* block = place(staging, len);
    if (!block) return false;
    anim_ = reinterpret_cast<AnimFn>(block);
    return true;
}

void MoonLive::free() {
    if (code_) platform::freeExec(code_, codeCap_);
    code_ = nullptr;
    codeCap_ = 0;
    codeLen_ = 0;
    fn_ = nullptr;
    anim_ = nullptr;
}

}  // namespace mm::moonlive
