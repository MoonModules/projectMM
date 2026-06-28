#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"
#include "moonlive_asm_riscv.h"

#include <cstring>

// MoonLive RISC-V backend (ESP32-P4) — lower a neutral IR program to RV32 bytes by driving the
// RISC-V assembler. The device counterpart of the Xtensa/host lowerings: same IR + the same
// StoreElem/FillElems inline ops; only the assembler differs. Host args: kArg0=buf, kArg1=nLights,
// kArg2=cpl, kArg3=t.

#if defined(__riscv)

namespace mm::moonlive {

namespace {
Reg reg(VReg v) { return static_cast<Reg>(v); }
}

size_t lowerToBytes(const IrProgram& ir, uint8_t* out, size_t cap) {
    // StoreElem folds the address into the index vreg (no scratch); FillElems needs two.
    if (!out || cap == 0 || ir.vregsUsed + 2 > kRegCount) return 0;
    const Reg sCtr  = static_cast<Reg>(ir.vregsUsed);
    const Reg sAddr = static_cast<Reg>(ir.vregsUsed + 1);

    RiscvAssembler a;

    for (uint8_t i = 0; i < ir.count; i++) {
        const IrInst& op = ir.ops[i];
        switch (op.op) {
            case IrOp::Const:  a.movImm(reg(op.dst), op.imm); break;
            case IrOp::Add:    a.addReg(reg(op.dst), reg(op.a), reg(op.b)); break;
            case IrOp::AddImm: a.addImm(reg(op.dst), reg(op.a), op.imm); break;
            case IrOp::Mul:    a.mulReg(reg(op.dst), reg(op.a), reg(op.b)); break;
            case IrOp::LoadCtrl: a.load8(reg(op.dst), reg(kArg4), op.imm); break;  // dst = ctrls[imm] (a4 = kArg4)
            case IrOp::Call:
                // The IR carries the host's function pointer (the light TU's random16), valid in
                // the single flashed image — call it directly, same as the other backends.
                if (!op.callFn) return 0;
                a.call(reg(op.dst), reg(op.a), reinterpret_cast<const void*>(op.callFn));
                break;
            case IrOp::Inline:
                switch (op.inlineOp) {
                    case InlineOp::StoreElem: {
                        Label skip = a.newLabel();
                        a.branchGeU(reg(op.a), reg(kArg1), skip);
                        a.mulReg(reg(op.a), reg(op.a), reg(kArg2));    // index = index*cpl
                        a.store8(reg(kArg0), reg(op.a), reg(op.b));
                        a.addImm(reg(op.a), reg(op.a), 1); a.store8(reg(kArg0), reg(op.a), reg(op.c));
                        a.addImm(reg(op.a), reg(op.a), 1); a.store8(reg(kArg0), reg(op.a), reg(op.d));
                        a.bind(skip);
                        break;
                    }
                    case InlineOp::FillElems: {
                        Label done = a.newLabel(), top = a.newLabel();
                        a.movImm(sCtr, 0);
                        a.branchIfZero(reg(kArg1), done);
                        a.bind(top);
                        a.mulReg(sAddr, sCtr, reg(kArg2));
                        a.store8(reg(kArg0), sAddr, reg(op.a));
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.b));
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.c));
                        a.addImm(sCtr, sCtr, 1);
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

#endif  // __riscv
