#include "core/moonlive/moonlive_emit.h"

#include <cstring>

// MoonLive ESP32 (Xtensa LX6/LX7) backend (§3.2) — the load-bearing hardware codegen.
// Emits the Stage-1a fixed-colour fill as Xtensa machine code; the engine copies it into
// IRAM (platform::writeExec) and calls it through FillFn. This is the path the bench run
// validates: native code we generated, executing in the render tick, writing the buffer.
//
//   void fill(uint8_t* buf, uint32_t nLights, uint8_t cpl)   // windowed ABI: a2,a3,a4
//   for (i=0; i<nLights; i++) { buf[i*cpl+0]=r; buf[i*cpl+1]=g; buf[i*cpl+2]=b; }
//
// The template is the verbatim assembler output (xtensa-esp32s3-elf-as, verified by
// objdump — see the disassembly in the plan / commit). Xtensa is little-endian with mixed
// 24-bit and 16-bit (narrow) instructions; the windowed prologue/epilogue is `entry`/`retw`
// so a plain C function pointer from windowed-ABI code calls it correctly. The colour bytes
// are the immediate byte of three wide `movi` instructions (forced wide so all three patch
// identically), at template offsets kR/kG/kB.

namespace mm::moonlive {

// Disassembly (offsets in the template below):
//   00: entry  a1, 32          (36 41 00)
//   03: beqz.n a3, .done       (9c d3)        nLights==0 -> return
//   05: movi.n a7, 0           (0c 07)        off = 0
//   07: movi.n a8, 0           (0c 08)        i = 0
//   09: movi   a5, R           (52 a0 NN)     ← R at offset 0x0b
//   0c: movi   a6, G           (62 a0 NN)     ← G at offset 0x0e
//   0f: movi   a9, B           (92 a0 NN)     ← B at offset 0x11
//   12: add.n  a10, a2, a7     (7a a2)        .loop: ptr = buf + off
//   14: s8i    a5, a10, 0      (52 4a 00)     ptr[0] = R
//   17: s8i    a6, a10, 1      (62 4a 01)     ptr[1] = G
//   1a: s8i    a9, a10, 2      (92 4a 02)     ptr[2] = B
//   1d: add.n  a7, a7, a4      (4a 77)        off += cpl
//   1f: addi.n a8, a8, 1       (1b 88)        i++
//   21: bne    a8, a3, .loop   (37 98 ed)     i != nLights -> loop
//   24: retw.n                 (1d f0)        .done:
static const uint8_t kXtensaFill[] = {
    0x36, 0x41, 0x00,        // entry a1, 32
    0x9c, 0xd3,              // beqz.n a3, .done
    0x0c, 0x07,              // movi.n a7, 0
    0x0c, 0x08,              // movi.n a8, 0
    0x52, 0xa0, 0x00,        // movi a5, R   (offset 0x0b)
    0x62, 0xa0, 0x00,        // movi a6, G   (offset 0x0e)
    0x92, 0xa0, 0x00,        // movi a9, B   (offset 0x11)
    0x7a, 0xa2,              // add.n a10, a2, a7
    0x52, 0x4a, 0x00,        // s8i a5, a10, 0
    0x62, 0x4a, 0x01,        // s8i a6, a10, 1
    0x92, 0x4a, 0x02,        // s8i a9, a10, 2
    0x4a, 0x77,              // add.n a7, a7, a4
    0x1b, 0x88,              // addi.n a8, a8, 1
    0x37, 0x98, 0xed,        // bne a8, a3, .loop
    0x1d, 0xf0,              // retw.n
};
static constexpr size_t kR = 0x0b, kG = 0x0e, kB = 0x11;   // colour-immediate byte offsets

size_t emitFill(uint8_t* out, size_t cap, uint8_t r, uint8_t g, uint8_t b) {
    if (cap < sizeof(kXtensaFill)) return 0;
    std::memcpy(out, kXtensaFill, sizeof(kXtensaFill));
    out[kR] = r;
    out[kG] = g;
    out[kB] = b;
    return sizeof(kXtensaFill);
}

// Xtensa animated fill (assembled from anim_xt.s, verified by objdump). The 4th windowed-ABI
// arg `t` arrives in a5; red = (t>>3)&0xFF is computed at runtime, green=0, blue=64. Nothing
// to patch — the colour is derived from `t`, so the SAME code animates as the host feeds a
// changing elapsed() each tick.
//   00: entry a1,32           06: srli a6,a5,3 (red=t>>3)   0e: movi.n a7,0 (green)
//   03: beqz.n a3,.done        08: movi a7,0xff             10: movi.n a8,64 (blue)
//                              0b: and a6,a6,a7              12: movi.n a9,0 (off)  14: movi.n a11,0 (i)
//   16: add.n a10,a2,a9  18: s8i a6 +0  1b: s8i a7 +1  1e: s8i a8 +2  21: add.n a9,a9,a4
//   23: addi.n a11,a11,1  25: bne a11,a3,.loop   28: retw.n
// Bytes copied VERBATIM from the assembler's binary output (objcopy of anim_xt.o) — NOT
// hand-transcribed from the disassembly, because Xtensa's mixed narrow/wide instructions and
// byte order make per-instruction hand-grouping error-prone (a transcription typo here caused
// a StoreProhibited crash on the S3 before this was regenerated from the raw blob).
static const uint8_t kXtensaAnim[] = {
    0x36, 0x41, 0x00, 0xac, 0x13, 0x50, 0x63, 0x41, 0x72, 0xa0, 0xff, 0x70,
    0x66, 0x10, 0x0c, 0x07, 0x4c, 0x08, 0x0c, 0x09, 0x0c, 0x0b, 0x9a, 0xa2,
    0x62, 0x4a, 0x00, 0x72, 0x4a, 0x01, 0x82, 0x4a, 0x02, 0x4a, 0x99, 0x1b,
    0xbb, 0x37, 0x9b, 0xed, 0x1d, 0xf0,
};

size_t emitAnimatedFill(uint8_t* out, size_t cap) {
    if (cap < sizeof(kXtensaAnim)) return 0;
    std::memcpy(out, kXtensaAnim, sizeof(kXtensaAnim));
    return sizeof(kXtensaAnim);
}

}  // namespace mm::moonlive
