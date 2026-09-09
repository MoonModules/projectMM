#include "core/moonlive/MoonLive.h"
#include <cstdio>
#include "core/moonlive/MoonLiveCompiler.h"
#include "platform/platform.h"

namespace mm::moonlive {


// Drop the prior compilation's CODE (the exec block + the typed fn pointers), but NOT the control
// arena — the arena's address must survive a recompile so a control pointer the binding bound to
// a slot stays valid (the arena only ever grows; see ensureArena). free() additionally releases
// the arena (full release).
void MoonLive::freeCode() {
    if (code_) platform::freeExec(code_, codeCap_);
    code_ = nullptr;
    codeCap_ = 0;
    codeLen_ = 0;
    fn_ = nullptr;
    anim_ = nullptr;
    ctrl_ = nullptr;
    // Likewise the error position. Only a PARSE failure has one, and it is set by the caller right
    // after this returns; the later failures (no control memory, codegen refused) have no offset of
    // their own, and without this they would report the position of whatever failed last. An editor
    // would then mark a line that has nothing to do with the error on screen.
    errorPos_ = 0;
    hasErrorPos_ = false;
    // The entry table describes code that no longer exists. Left behind, entry() would hand a
    // binding an address into a freed block: the same stale-state trap the control arena has.
    entryCount_ = 0;
    // The declared controls go with it: a control record describes the program that just went
    // away, and running its defineControls() is what will publish the next set. Dropping them here
    // is also what makes a FAILED recompile safe, since the records would otherwise outlive the
    // code and the pool their names point into.
    //
    // The string pool itself is NOT cleared here. freeCode runs mid-compile (place() calls it), so
    // clearing would wipe the labels of the program still published while the next one is being
    // built. It is reclaimed when the next compile interns into it from offset zero, which is the
    // ordinary arena discipline: one program's strings at a time.
    controlCount_ = 0;
    stringLen_ = 0;
}

// Copy `len` already-emitted bytes into a fresh exec block. writeExec hides the ISA quirks
// (IRAM's 32-bit-store-only rule, the I-cache sync), so the engine stays target-agnostic.
// Returns the block (ready to call) or nullptr on failure (error_ set, prior state freed).
void* MoonLive::place(const uint8_t* staged, size_t len) {
    freeCode();   // drop any prior compilation's code — (re)compile is a clean re-emit (arena kept)
    if (len == 0) { error_ = "emit failed"; return nullptr; }
    // Allocate only what was emitted, word-rounded (writeExec stores 32-bit words on IRAM) — a fill
    // is ~50 bytes, a four-call setRGB ~600. The staging buffer is sized from the script's token
    // count (codeCapFor); the live exec block is sized to what the program actually emitted.
    size_t cap = (len + 3) & ~size_t(3);
    void* block = platform::allocExec(cap);
    if (!block) { error_ = "no executable memory"; return nullptr; }
    platform::writeExec(block, staged, len);
    code_ = block;
    codeCap_ = cap;
    codeLen_ = len;
    error_ = "";
    errorPos_ = 0;
    return block;
}

// The emitted-code staging buffer, on the HEAP.
//
// It was `uint8_t staging[kCodeCap]` — 2 KB of stack, in a call chain that also holds the
// assembler's own 2 KB buffer and its tables: ~4.7 KB in one go. On a classic ESP32 that overflowed
// the task the compile runs on, and the fault surfaced as `Double exception` inside
// _xt_context_save (the handler faulting while saving context) with LBEG pointing back into
// MoonLive::compile — a crash on the HTTP task from naming a script. Compilation is cold path, so
// the allocation costs nothing that matters, and this is the same reasoning that moved IrProgram's
// op array off the stack.
// Sized per compile rather than to kCodeCap: that constant is now the SANITY bound (16 KB), and
// allocating it for every script would trade one wall for a heap cost on the smallest script.
namespace {
struct Staging {
    explicit Staging(size_t bytes) : p(static_cast<uint8_t*>(platform::alloc(bytes))), n(bytes) {}
    ~Staging() { platform::free(p); }
    Staging(const Staging&) = delete;              // owns a buffer; a copy would double-free
    Staging& operator=(const Staging&) = delete;
    explicit operator bool() const { return p != nullptr; }
    uint8_t* p;
    size_t   n;
};
}  // namespace

bool MoonLive::compile(uint8_t r, uint8_t g, uint8_t b) {
    // A fixed blob with no source to measure, so codeCapFor(0) gives its 256-byte floor — emitFill
    // emits a few dozen bytes and the exec block is allocated to the real length.
    Staging staging(codeCapFor(0));
    if (!staging) { error_ = "no memory to compile"; return false; }
    size_t len = emitFill(staging.p, staging.n, r, g, b);
    void* block = place(staging.p, len);
    if (!block) return false;
    fn_ = reinterpret_cast<FillFn>(block);
    return true;
}

bool MoonLive::compile(const char* source, const BuiltinTable& table, const SysVarTable& sysvars) {
    Staging staging(codeCapFor(countTokens(source)));
    if (!staging) { freeCode(); error_ = "no memory to compile"; return false; }
    // strings_ is passed so a string literal is interned into memory that outlives the compile:
    // the emitted code carries pointers into it, and the source buffer is freed the moment this
    // returns. NOT cleared here: freeCode() owns that, because a control record published by the
    // previous program still points into this pool. Zeroing before a compile that then FAILS left
    // every published control named "": name-keyed persistence and `POST /api/control` both go
    // through that name, so a broken script silently unbound the user's own sliders.
    CompileResult cr = compileSource(source, table, sysvars, staging.p, staging.n,
                                     nullptr, nullptr, strings_, CompileResult::kStringPool);
    // The diagnostic AND where it happened: an editor can only mark the line if it is told one,
    // and the parser has already computed the offset (Parser::fail records lex.col()).
    // errorCol is ONE-based (Lexer::col() adds 1), and every consumer counts from zero: the editor
    // slices the source up to this offset to find the line. Converted here, at the boundary between
    // the compiler's convention and everyone else's, rather than in each reader.
    if (!cr.ok) {
        freeCode();
        // The allocator's refusal is the one error worth the numbers behind it (which guard, and the
        // budget it saw): they are what turned "codegen failed" into a diagnosis on the bench. One
        // FILE-static line, formatted on this cold path only: not a buffer per engine (there are
        // several), not one per result (a temporary; a pointer into it dangled). Two modules failing
        // this way at once would share the line, which is a diagnostic corner, not a data path.
        if (cr.error == kSpillRefused) {
            // 160: the literal is 63 bytes and each of six uint8 fields can print three digits, which
            // GCC's -Wformat-truncation proved 112 could clip (clang says nothing, the CI gap).
            static char detail[160];
            const SpillDetail& d = spillDetail();
            std::snprintf(detail, sizeof(detail), "%s (guard %u, avail %u, temps %u, vregs %u, slots %u, spilled %u)",
                          kSpillRefused, d.guard, d.avail, d.temps, d.vregs, d.slots, d.spilled);
            error_ = detail;
        } else {
            error_ = cr.error;
        }
        errorPos_ = cr.errorCol > 0 ? static_cast<uint16_t>(cr.errorCol - 1) : 0;
        hasErrorPos_ = true;
        return false;
    }
    // Allocate the control arena (fixed address) and seed new slots, BEFORE publishing the control
    // set — ensureArena reads the previous controlCount_ to know which slots are new.
    // Seeded from the MEMBERS, not the controls: a member the UI never shows still has an
    // initializer, and reading it before anything wrote would give 0 rather than what the script
    // declared. A control is one of these members surfaced, so seeding members covers both.
    if (!ensureArena(cr.members, cr.memberCount)) { freeCode(); error_ = "no control memory"; return false; }
    // Place the code. Only after it succeeds do we publish the new control set — a failed place()
    // must not leave declaredControls() advertising controls for code that isn't running.
    void* block = place(staging.p, cr.len);
    if (!block) return false;                                      // controlCount_/controls_ unchanged
    // Copy the entry table, names included: a CompileResult's `name` points into the source text,
    // which the caller frees as soon as this returns.
    entryCount_ = cr.entryCount < kMaxEntryPoints ? cr.entryCount : kMaxEntryPoints;
    for (uint8_t i = 0; i < entryCount_; i++) {
        const uint8_t n = cr.entries[i].nameLen < kMaxEntryName ? cr.entries[i].nameLen : kMaxEntryName;
        for (uint8_t j = 0; j < n; j++) entryNames_[i][j] = cr.entries[i].name[j];
        entryNames_[i][n] = '\0';
        // The DECLARED return type travels with the entry: without it every function reads as
        // Void here and runValue refuses to answer for any of them.
        entries_[i] = {entryNames_[i], n, cr.entries[i].offset, cr.entries[i].ret};
    }
    stringLen_ = cr.stringLen;
    ctrl_ = reinterpret_cast<CtrlFn>(block);
    return true;
}

// Ensure the control arena exists and seed newly-declared slots. The arena is allocated ONCE at
// full kArenaBytes capacity and never reallocated, so its address, and every control pointer the
// binding bound to a slot — is fixed for the engine's lifetime (the stable-slot contract
// controlSlot() promises; a recompile that adds a control must not move a pointer the previous
// defineControls already published). kArenaBytes is a handful; the up-front allocation is
// cheaper than the move-and-rebind it avoids. A NEW slot (beyond the previous count) is seeded
// from its declared default; an EXISTING slot keeps its live value (a source edit that keeps the
// control preserves the slider position). Returns false on alloc failure.
bool MoonLive::ensureArena(const DeclaredControl* decls, uint8_t count) {
    if (!ctrlArena_) {
        ctrlArena_ = static_cast<uint8_t*>(platform::alloc(kArenaBytes));
        if (!ctrlArena_) return false;
        for (uint8_t i = 0; i < kArenaBytes; i++) ctrlArena_[i] = 0;
    }
    // A NEW slot takes its declared initializer; one holding the SAME member keeps its live value,
    // so a source edit that keeps a control does not snap its slider back to the default. "Same"
    // is offset AND name: a member inserted at the top of the class shifts every later declaration
    // to a new offset, and each of those is a different member now occupying a seeded byte, so it
    // must take its own initializer rather than inherit the previous occupant's value.
    uint64_t seeding = 0;
    uint8_t  kept = 0;                 // rows written this pass; the table is per MEMBER
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t off = decls[i].offset;
        // Bounded by the SCRIPT's region, which is what the mask and the name table cover: a member
        // never sits above it, and the parser already refuses one that would.
        if (off >= kCtrlBytes) continue;
        // The declared name is a SPAN of the source (nameLen, no terminator), so it is compared
        // and stored length-bounded: strcmp would read past it into the rest of the script.
        const uint8_t n = decls[i].nameLen < kSeedNameLen - 1 ? decls[i].nameLen
                                                             : uint8_t(kSeedNameLen - 1);
        // Same MEMBER means same (offset, name). The offset alone is not identity: inserting a
        // member at the top of a class shifts every later one down, and each then occupies a byte
        // that was seeded for something else.
        const SeededMember* prev = nullptr;
        if ((seeded_ >> off) & 1ull)
            for (uint8_t k = 0; k < seededCount_; k++)
                if (seededName_[k].offset == off) { prev = &seededName_[k]; break; }
        // Same NAME, same SHAPE. A widened scalar or a grown array keeps its name and offset, and
        // reusing its bytes on that basis would leave the new extent holding the old program's
        // values (see SeededMember).
        const bool same = prev && std::strncmp(prev->name, decls[i].name, n) == 0 &&
                          prev->name[n] == '\0' &&
                          prev->type == decls[i].type && prev->count == decls[i].count;
        if (!same) {
            // Seed the member's WHOLE extent: every element, at its width, little-endian to
            // match every backend's load. Writing only the first element left an ARRAY holding
            // the previous program's bytes from element 1 on, which is what "an array starts at
            // zero" has to mean; writing only the low byte left the rest of a wider member stale,
            // which turned `int neg = -100;` into 156 (the sign bytes never reached the slot).
            //
            // A SCALAR is one 4-byte slot; an ARRAY packs at its element width. Both spelled here
            // as "write w bytes per element", so the two cases are one loop rather than two.
            const uint8_t w = decls[i].count > 1 ? ctrlWidth(decls[i].type)
                                                 : ctrlSlotBytes(decls[i].type);
            const uint32_t v = static_cast<uint32_t>(decls[i].def);
            for (uint16_t e = 0; e < decls[i].count; e++) {
                const uint16_t at = uint16_t(off + e * w);
                if (at + w > kCtrlBytes) break;                 // the parser bounds it; belt and braces
                for (uint8_t b = 0; b < w; b++)
                    ctrlArena_[at + b] = static_cast<uint8_t>((v >> (8 * b)) & 0xff);
            }
        }
        if (kept < kMaxCtrls) {
            seededName_[kept].offset = off;
            seededName_[kept].type   = decls[i].type;
            seededName_[kept].count  = static_cast<uint8_t>(decls[i].count);
            for (uint8_t c = 0; c < n; c++) seededName_[kept].name[c] = decls[i].name[c];
            seededName_[kept].name[n] = '\0';
            kept++;
        }
        seeding |= 1ull << off;
    }
    seeded_ = seeding;   // a member the new script dropped is unseeded: its byte reseeds if it returns
    seededCount_ = kept;
    return true;
}

bool MoonLive::compileAnimated() {
    Staging staging(codeCapFor(0));   // a fixed blob, like compile(r,g,b) — no source to measure
    if (!staging) { error_ = "no memory to compile"; return false; }
    size_t len = emitAnimatedFill(staging.p, staging.n);
    void* block = place(staging.p, len);
    if (!block) return false;
    anim_ = reinterpret_cast<AnimFn>(block);
    return true;
}

void MoonLive::free() {
    freeCode();                       // exec block + fn pointers
    if (ctrlArena_) platform::free(ctrlArena_);   // full release also releases the control arena
    ctrlArena_ = nullptr;
    controlCount_ = 0;
    // The seeded-slot count goes with the arena it describes. Left behind, the next compile would
    // treat every member as one it had already seeded and skip the initializers, so a script would
    // start every value at zero instead of what it declared.
    seeded_ = 0;
    seededCount_ = 0;
}

}  // namespace mm::moonlive
