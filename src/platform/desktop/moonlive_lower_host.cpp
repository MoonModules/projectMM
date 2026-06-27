#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"
#include "moonlive_asm_host.h"

#include <cstring>

// MoonLive host backend — lower a typed IR program to machine bytes by driving the host
// assembler. The IR is neutral; this file knows how each IR op becomes assembler calls. The
// host arguments arrive in the lowest vregs (the light host assigns kArg0=buf, kArg1=nLights,
// kArg2=cpl, kArg3=t — the only place this backend assumes the LED layout, and only to
// implement the StoreElem/FillElems inline ops the host registered).
//
// Vregs map 1:1 onto the assembler's Reg handles. The inline ops use a couple of scratch
// registers ABOVE the program's high-water mark (the parser never allocates them), so they
// don't clobber a live vreg.

namespace mm::moonlive {

#if defined(__aarch64__)   // the host assembler is implemented for arm64 only (see moonlive_asm_host.cpp)

namespace {
Reg reg(VReg v) { return static_cast<Reg>(v); }
}

size_t lowerToBytes(const IrProgram& ir, uint8_t* out, size_t cap) {
    // Reserve three scratch regs above the program's vregs for the inline ops' temps.
    if (!out || cap == 0 || ir.vregsUsed + 3 > kRegCount) return 0;
    const Reg sOff  = static_cast<Reg>(ir.vregsUsed);        // base byte offset of the current light
    const Reg sCtr  = static_cast<Reg>(ir.vregsUsed + 1);    // loop counter
    const Reg sAddr = static_cast<Reg>(ir.vregsUsed + 2);    // per-channel address (off, off+1, off+2)

    HostAssembler a;

    for (uint8_t i = 0; i < ir.count; i++) {
        const IrInst& op = ir.ops[i];
        switch (op.op) {
            case IrOp::Const:  a.movImm(reg(op.dst), op.imm); break;
            case IrOp::Add:    a.addReg(reg(op.dst), reg(op.a), reg(op.b)); break;
            case IrOp::AddImm: a.addImm(reg(op.dst), reg(op.a), op.imm); break;
            case IrOp::Mul:    a.mulReg(reg(op.dst), reg(op.a), reg(op.b)); break;
            case IrOp::Call:
                if (!op.callFn) return 0;
                a.call(reg(op.dst), reg(op.a), reinterpret_cast<const void*>(op.callFn));
                break;
            case IrOp::Inline:
                switch (op.inlineOp) {
                    case InlineOp::StoreElem: {
                        // setRGB(index=a, r=b, g=c, b=d): bounds-guard, addr = index*cpl, store 3.
                        Label skip = a.newLabel();
                        a.cmp(reg(op.a), reg(kArg1));         // index vs nLights
                        a.branchIf(Cond::Hs, skip);           // index >= nLights → skip
                        a.mulReg(sAddr, reg(op.a), reg(kArg2));   // addr = index * cpl
                        a.store8(reg(kArg0), sAddr, reg(op.b));   // buf[addr+0] = r
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.c));  // +1 = g
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.d));  // +2 = b
                        a.bind(skip);
                        break;
                    }
                    case InlineOp::FillElems: {
                        // fill(r=a, g=b, b=c): for i in 0..nLights { buf[i*cpl+0..2] = r,g,b }.
                        // sOff = byte base of the current light; sAddr = sOff/+1/+2 per channel
                        // (a fresh copy each light so the +1/+2 never corrupt sOff); sOff += cpl.
                        Label done = a.newLabel(), top = a.newLabel();
                        a.movImm(sOff, 0);                    // off = 0
                        a.movImm(sCtr, 0);                    // i = 0
                        a.branchIfZero(reg(kArg1), done);     // nLights==0 → skip
                        a.bind(top);
                        a.addImm(sAddr, sOff, 0); a.store8(reg(kArg0), sAddr, reg(op.a));   // buf[off+0]=r
                        a.addImm(sAddr, sOff, 1); a.store8(reg(kArg0), sAddr, reg(op.b));   // buf[off+1]=g
                        a.addImm(sAddr, sOff, 2); a.store8(reg(kArg0), sAddr, reg(op.c));   // buf[off+2]=b
                        a.addReg(sOff, sOff, reg(kArg2));     // off += cpl   (general stride)
                        a.addImm(sCtr, sCtr, 1);              // i++
                        a.cmp(sCtr, reg(kArg1));
                        a.branchIf(Cond::Lo, top);
                        a.bind(done);
                        break;
                    }
                }
                break;
            default: break;   // Loop/LoopEnd/Bounds/BoundsEnd are not emitted by the parser now
        }
    }
    a.ret();
    a.finalize();
    if (a.overflowed() || a.size() > cap) return 0;
    std::memcpy(out, a.bytes(), a.size());
    return a.size();
}

#else   // unsupported host ISA (e.g. Windows x64) — degrade: no codegen, compile fails cleanly.

size_t lowerToBytes(const IrProgram&, uint8_t*, size_t) { return 0; }

#endif

}  // namespace mm::moonlive
