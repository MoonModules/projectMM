#include "core/moonlive/moonlive_emit.h"

#include <cstring>

// MoonLive desktop backend (§3.2) — emits the fixed-colour fill as host machine code (arm64
// or x86-64, chosen at compile time). The desktop backend lets a unit test EXECUTE generated
// code in-process — proving allocExec → writeExec → call works off-hardware — and exercises
// the engine/binding API the same way the device backends do. Hand-encoding the loop here is
// the same exercise the Xtensa/RISC-V backends do; the on-device backends are validated by
// the hardware run, this one keeps CI honest.
//
// The routine implements: void fill(uint8_t* buf, uint32_t nLights, uint8_t cpl)
//   for (i=0; i<nLights; i++) { buf[i*cpl+0]=r; buf[i*cpl+1]=g; buf[i*cpl+2]=b; }
// The R/G/B bytes are patched into the template at known offsets — the rest is fixed.

namespace mm::moonlive {

#if defined(__aarch64__)

// arm64 template (assembled from fill_arm64.s, verified with clang/objdump). 18 words.
// buf=x0, nLights=w1, cpl=w2. R/G/B live in `mov w4/w5/w6, #imm` at word indices 4,5,6.
static const uint32_t kArm64[] = {
    0x34000221,  // cbz   w1, .done
    0xd2800003,  // mov   x3, #0          (byte offset)
    0x12001c42,  // and   w2, w2, #0xff
    0x53001c42,  // uxtb  w2, w2          (cpl stride)
    0x52800004,  // mov   w4, #0          ← R patched: | (r<<5)
    0x52800005,  // mov   w5, #0          ← G patched: | (g<<5)
    0x52800006,  // mov   w6, #0          ← B patched: | (b<<5)
    0xd2800007,  // mov   x7, #0          (light index)
    0x38236804,  // strb  w4, [x0, x3]    .loop:
    0x91000468,  // add   x8, x3, #1
    0x38286805,  // strb  w5, [x0, x8]
    0x91000868,  // add   x8, x3, #2
    0x38286806,  // strb  w6, [x0, x8]
    0x910004e7,  // add   x7, x7, #1
    0x8b020063,  // add   x3, x3, x2
    0xeb0100ff,  // cmp   x7, x1
    0x54ffff03,  // b.lo  .loop          (-0x20)
    0xd65f03c0,  // ret                  .done:
};

size_t emitFill(uint8_t* out, size_t cap, uint8_t r, uint8_t g, uint8_t b) {
    if (!out || cap < sizeof(kArm64)) return 0;
    uint32_t code[sizeof(kArm64) / 4];
    std::memcpy(code, kArm64, sizeof(kArm64));
    // Patch the colour immediates: mov wN,#imm encodes imm at bits [20:5]; the base word
    // has imm=0 so OR-ing (imm<<5) sets it cleanly.
    code[4] = 0x52800004u | (static_cast<uint32_t>(r) << 5);
    code[5] = 0x52800005u | (static_cast<uint32_t>(g) << 5);
    code[6] = 0x52800006u | (static_cast<uint32_t>(b) << 5);
    std::memcpy(out, code, sizeof(code));
    return sizeof(code);
}

// arm64 animated fill (assembled from anim_arm64.s): red = (t>>3)&0xFF, green=0, blue=64.
// t arrives in w3; nothing to patch — the colour is computed from the runtime arg.
static const uint32_t kArm64Anim[] = {
    0x34000241,  // cbz   w1, .done
    0x53037c64,  // lsr   w4, w3, #3      red = t>>3
    0x12001c84,  // and   w4, w4, #0xff
    0x52800005,  // mov   w5, #0          green
    0x52800806,  // mov   w6, #64         blue
    0xd2800003,  // mov   x3, #0          off
    0x12001c42,  // and   w2, w2, #0xff
    0x53001c42,  // uxtb  w2, w2          stride
    0xd2800007,  // mov   x7, #0          i
    0x38236804,  // strb  w4, [x0, x3]    .loop:
    0x91000468,  // add   x8, x3, #1
    0x38286805,  // strb  w5, [x0, x8]
    0x91000868,  // add   x8, x3, #2
    0x38286806,  // strb  w6, [x0, x8]
    0x910004e7,  // add   x7, x7, #1
    0x8b020063,  // add   x3, x3, x2
    0xeb0100ff,  // cmp   x7, x1
    0x54ffff03,  // b.lo  .loop
    0xd65f03c0,  // ret   .done:
};

size_t emitAnimatedFill(uint8_t* out, size_t cap) {
    if (!out || cap < sizeof(kArm64Anim)) return 0;
    std::memcpy(out, kArm64Anim, sizeof(kArm64Anim));
    return sizeof(kArm64Anim);
}

#elif defined(__x86_64__) && !defined(_WIN32)

// x86-64 SysV ABI (Linux/macOS) — args in rdi/rsi/rdx. The Windows x64 ABI uses
// rcx/rdx/r8/r9 instead, so this blob is wrong there; _WIN32 is excluded above and falls to
// the #error until a Win64 template is added (no Windows desktop target ships today).
// (assembled from fill_x64.s, verified with clang/objdump). buf=rdi, nLights=esi, cpl=dl.
// R/G/B are the immediate byte of each `movb` at offsets 0x11/0x17/0x1d.
static const uint8_t kX64[] = {
    0x85, 0xf6,                         // test  esi, esi
    0x74, 0x25,                         // je    .done (+0x25)
    0x0f, 0xb6, 0xca,                   // movzx ecx, dl          (stride)
    0x4d, 0x31, 0xc0,                   // xor   r8, r8           (i)
    0x4d, 0x31, 0xc9,                   // xor   r9, r9           (off)
    0x42, 0xc6, 0x04, 0x0f, 0x11,       // mov   byte [rdi+r9], R     ← off 0x11
    0x42, 0xc6, 0x44, 0x0f, 0x01, 0x22, // mov   byte [rdi+r9+1], G   ← off 0x17
    0x42, 0xc6, 0x44, 0x0f, 0x02, 0x33, // mov   byte [rdi+r9+2], B   ← off 0x1d
    0x49, 0xff, 0xc0,                   // inc   r8
    0x49, 0x01, 0xc9,                   // add   r9, rcx
    0x49, 0x39, 0xf0,                   // cmp   r8, rsi
    0x72, 0xe4,                         // jb    .loop (-0x1c)
    0xc3,                               // ret   .done:
};

size_t emitFill(uint8_t* out, size_t cap, uint8_t r, uint8_t g, uint8_t b) {
    if (!out || cap < sizeof(kX64)) return 0;
    std::memcpy(out, kX64, sizeof(kX64));
    out[0x11] = r;
    out[0x17] = g;
    out[0x1d] = b;
    return sizeof(kX64);
}

// x86-64 animated fill (assembled from anim_x64.s): red = (t>>3)&0xFF, green=0, blue=64.
// t arrives in ecx; nothing to patch — the colour is computed from the runtime arg.
static const uint8_t kX64Anim[] = {
    0x85, 0xf6,                         // test  esi, esi
    0x74, 0x2d,                         // je    .done (+0x2d)
    0x89, 0xc8,                         // mov   eax, ecx        (t)
    0xc1, 0xe8, 0x03,                   // shr   eax, 3          red = t>>3
    0x0f, 0xb6, 0xc0,                   // movzx eax, al
    0x44, 0x0f, 0xb6, 0xd2,             // movzx r10d, dl        (stride)
    0x4d, 0x31, 0xc0,                   // xor   r8, r8          (i)
    0x4d, 0x31, 0xc9,                   // xor   r9, r9          (off)
    0x42, 0x88, 0x04, 0x0f,             // mov   byte [rdi+r9], al    (red, dynamic)
    0x42, 0xc6, 0x44, 0x0f, 0x01, 0x00, // mov   byte [rdi+r9+1], 0   (green)
    0x42, 0xc6, 0x44, 0x0f, 0x02, 0x40, // mov   byte [rdi+r9+2], 64  (blue)
    0x49, 0xff, 0xc0,                   // inc   r8
    0x4d, 0x01, 0xd1,                   // add   r9, r10
    0x49, 0x39, 0xf0,                   // cmp   r8, rsi
    0x72, 0xe5,                         // jb    .loop (-0x1b)
    0xc3,                               // ret   .done:
};

size_t emitAnimatedFill(uint8_t* out, size_t cap) {
    if (!out || cap < sizeof(kX64Anim)) return 0;
    std::memcpy(out, kX64Anim, sizeof(kX64Anim));
    return sizeof(kX64Anim);
}

#else

// Unsupported host ISA/ABI (e.g. Windows x64 — rcx/rdx/r8/r9 + shadow space, its own template
// not written yet). Rather than break the build, MoonLive degrades: every emit returns 0, so
// MoonLive::compile reports "emit failed" and the scripted module runs dark — the same clean
// no-code path a too-large or unparseable script takes. The Windows desktop binary builds and
// runs; scripted effects are simply unavailable there until a Win64 backend lands.
size_t emitFill(uint8_t*, size_t, uint8_t, uint8_t, uint8_t) { return 0; }
size_t emitAnimatedFill(uint8_t*, size_t) { return 0; }

#endif

}  // namespace mm::moonlive
