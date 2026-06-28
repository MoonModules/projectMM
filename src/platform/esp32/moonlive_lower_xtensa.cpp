#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"
#include "moonlive_asm_xtensa.h"

#include <cstring>

// MoonLive Xtensa backend — lower a neutral IR program to Xtensa machine bytes by driving the
// Xtensa assembler. The device counterpart of moonlive_lower_host.cpp: same IR + the same
// StoreElem/FillElems inline ops the host registered; only the assembler/ABI differ. Built under
// __XTENSA__ (classic ESP32 / S3). The host args arrive as kArg0=buf, kArg1=nLights, kArg2=cpl,
// kArg3=t (the only LED-layout assumption, used to implement the inline ops).

#if defined(__XTENSA__)

namespace mm::moonlive {

namespace {
Reg reg(VReg v) { return static_cast<Reg>(v); }
}

size_t lowerToBytes(const IrProgram& ir, uint8_t* out, size_t cap) {
    // StoreElem needs no scratch (it folds the address into the index vreg); FillElems needs two
    // (counter + per-channel addr) above the program's vregs. Reserve the max either uses.
    if (!out || cap == 0 || ir.vregsUsed + 2 > kRegCount) return 0;
    const Reg sCtr  = static_cast<Reg>(ir.vregsUsed);       // FillElems loop counter
    const Reg sAddr = static_cast<Reg>(ir.vregsUsed + 1);   // FillElems per-channel address

    XtensaAssembler a;
    a.prologue();

    for (uint8_t i = 0; i < ir.count; i++) {
        const IrInst& op = ir.ops[i];
        switch (op.op) {
            case IrOp::Const:  a.movImm(reg(op.dst), op.imm); break;
            case IrOp::Add:    a.addReg(reg(op.dst), reg(op.a), reg(op.b)); break;
            case IrOp::AddImm: a.addImm(reg(op.dst), reg(op.a), op.imm); break;
            case IrOp::Mul:    a.mulReg(reg(op.dst), reg(op.a), reg(op.b)); break;
            case IrOp::LoadCtrl: a.load8(reg(op.dst), reg(kArg4), op.imm); break;  // dst = ctrls[imm] (a6 = kArg4)
            case IrOp::Call:
                if (!op.callFn) return 0;
                a.call(reg(op.dst), reg(op.a), reinterpret_cast<const void*>(op.callFn));
                break;
            case IrOp::Inline:
                switch (op.inlineOp) {
                    case InlineOp::StoreElem: {
                        // setRGB(index=a, r=b, g=c, b=d): bounds-guard, then fold the address
                        // INTO the index vreg (dead after) so no extra scratch is needed.
                        Label skip = a.newLabel();
                        a.branchGeU(reg(op.a), reg(kArg1), skip);     // index >= nLights → skip
                        a.mulReg(reg(op.a), reg(op.a), reg(kArg2));    // index = index * cpl  (= addr)
                        a.store8(reg(kArg0), reg(op.a), reg(op.b));    // store r
                        a.addImm(reg(op.a), reg(op.a), 1); a.store8(reg(kArg0), reg(op.a), reg(op.c));
                        a.addImm(reg(op.a), reg(op.a), 1); a.store8(reg(kArg0), reg(op.a), reg(op.d));
                        a.bind(skip);
                        break;
                    }
                    case InlineOp::FillElems: {
                        // fill(r=a, g=b, b=c): for i in 0..nLights { addr=i*cpl; buf[addr+0..2]=r,g,b }.
                        // Two scratch: sCtr (i), sAddr (the per-light address).
                        Label done = a.newLabel(), top = a.newLabel();
                        a.movImm(sCtr, 0);
                        a.branchIfZero(reg(kArg1), done);
                        a.bind(top);
                        a.mulReg(sAddr, sCtr, reg(kArg2));            // addr = i * cpl
                        a.store8(reg(kArg0), sAddr, reg(op.a));
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.b));
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.c));
                        a.addImm(sCtr, sCtr, 1);                       // i++
                        a.branchNe(sCtr, reg(kArg1), top);
                        a.bind(done);
                        break;
                    }
                }
                break;
            default: break;
        }
    }
    a.epilogue();
    a.finalize();
    if (a.overflowed() || a.size() > cap) return 0;
    std::memcpy(out, a.bytes(), a.size());
    return a.size();
}

}  // namespace mm::moonlive

#endif  // __XTENSA__
