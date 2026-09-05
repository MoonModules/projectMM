#include "core/moonlive/MoonLiveSpill.h"

// Linear-scan register allocation with spilling to the call frame. See MoonLiveSpill.h for why this
// lives in core rather than in each backend.
//
// The shape of the frame is the part that has to survive what comes next. MoonLive is gaining
// user-defined functions — with arguments, loops, and recursion — so the overflow storage is
// deliberately a CALL FRAME addressed through a frame pointer, not a global slot file: slot `n` is
// an offset from the frame the currently-executing routine owns, and a nested or recursive call
// pushes its own. A global file would work for one top-level program and would then have to be
// thrown away, because two activations of the same function would share the same slot and the inner
// one would clobber the outer's values.

namespace mm::moonlive {

namespace {

// Reload temps: registers held back from allocation so a spilled operand has somewhere to land for
// the one instruction that reads it. Four is the worst case, set by the widest op — the StoreElem
// inline reads a, b, c AND d. A dst reuses a source temp rather than claiming a fifth: every op here
// is one machine instruction that reads its sources and then writes its destination, so `mul d,a,b`
// with d aliasing a is well-defined on all three ISAs, and on a target with twelve registers the
// fifth would be a real cost.
constexpr uint8_t kMaxReloadTemps = 4;

// One value's live range, in op indices. `end` is the last index that mentions the vreg, AFTER loop
// extension. Sixteen bytes per vreg with kMaxVRegs = 32: a few hundred bytes of stack, small enough
// to stay a local on the 12 KB task the compile shares (a classic ESP32 has already been watchdog
// reset by this path; a heap allocation here would be a second failure mode for no gain).
struct Interval {
    uint16_t start = 0;
    uint16_t end = 0;
    VReg     vreg = 0;
    bool     live = false;      // does this vreg appear at all?
    bool     spilled = false;
    uint8_t  slot = 0;          // frame slot, when spilled
    VReg     assigned = 0;      // compacted register number, when not
};

// A loop, as op indices: the header (where the back edge lands) and the back edge itself. The
// grammar has no break, continue or goto, so a loop is EXACTLY a BranchNe whose label was bound
// earlier in the array — the op array is already in reverse postorder and no CFG has to be built.
// That is the one bespoke simplification here, and it carries the guard below: a branch structure
// that is not properly nested makes the pass refuse rather than allocate against a wrong interval,
// so adding `break` later fails loudly instead of miscompiling in silence.
struct Loop { uint16_t header; uint16_t back; };

// Every vreg an op READS. Written as one function so the interval builder and the rewriter cannot
// disagree about which operand fields an op uses — a mismatch there is exactly the bug that produces
// a value spilled but never reloaded.
uint8_t sourcesOf(const IrInst& in, VReg* out) {
    switch (in.op) {
        case IrOp::Const:                                   return 0;
        case IrOp::ConstPtr:                                return 0;   // an address, not a value
        case IrOp::Reload:                                  return 0;
        case IrOp::Label:                                   return 0;
        case IrOp::Mov:
        case IrOp::AddImm:
        case IrOp::Spill:      out[0] = in.a;               return 1;
        // A `return` reads its value ONLY when it carries one: `imm` says so, because it is not a
        // register field and the rewriter renumbers every vreg reported here. A bare return reports
        // nothing, so its `a` stays whatever the emitter left and the lowering never reads it.
        case IrOp::Ret:        if (!in.imm) return 0;
                               out[0] = in.a;               return 1;
        case IrOp::LoadCtrl:   out[0] = kArg4;              return 1;   // reads the arena pointer
        // A member STORE reads the VALUE being written, and nothing else. The arena pointer is
        // deliberately NOT reported, for the same reason LoadIdx/StoreIdx do not report it: the
        // rewriter below writes sources back POSITIONALLY, so listing kArg4 first shifts the value
        // into `b` and leaves `a` holding kArg4's register. Both lowerings read the value from
        // `op.a`, so every member assignment would store whatever that register held, the moment
        // the allocator rewrites anything. The pointer is reached through host(kArg4) at lowering
        // time and needs no live interval here.
        case IrOp::StoreCtrl:
        case IrOp::StoreCtrl32: out[0] = in.a; return 1;
        case IrOp::LoadCtrl32:  out[0] = kArg4;               return 1;   // reads the arena pointer
        // An indexed access reads its INDEX (and, for a store, the value). The arena pointer is
        // deliberately NOT reported: the rewriter below writes sources back POSITIONALLY (src[0]
        // into in.a, src[1] into in.b), so listing kArg4 first would shift every real operand one
        // place along, leaving the index in the value's field. LoadCtrl gets away with reporting
        // it because it has no other source and reads the pointer through host(kArg4); these ops
        // do the same, so kArg4 needs no live interval here either.
        case IrOp::LoadIdx:     out[0] = in.a;               return 1;
        case IrOp::StoreIdx:    out[0] = in.a; out[1] = in.b; return 2;
        // Shl/Sar carry their shift amount in `imm`, so the vreg source is the value alone; Mulhi
        // reads both operands exactly as Mul does.
        case IrOp::Shl:
        case IrOp::Shr:
        case IrOp::Sar:        out[0] = in.a;               return 1;
        case IrOp::Mulhi:      out[0] = in.a; out[1] = in.b; return 2;
        case IrOp::Add:
        case IrOp::Mul:
        case IrOp::BranchGe:
        case IrOp::BranchGeS:
        case IrOp::BranchNe:   out[0] = in.a; out[1] = in.b; return 2;
        // A Call reads NO registers. Its arguments were staged into consecutive frame slots by the
        // parser, so `imm` is their base and `b` is how MANY there are — a literal count, not a
        // vreg. Reporting a/b/c as sources gave the count a live interval and let the rewrite below
        // remap it into a register number; it survived only because a fixed ABI vreg maps to itself.
        case IrOp::Call:       return 0;
        // Nor does a call to the script's OWN function: `imm` is the callee's function NUMBER, not
        // a value, and the callee reads its arguments from frame slots exactly as a host built-in
        // does.
        case IrOp::CallScript: return 0;
        case IrOp::Inline:
            // The inline ops read every operand field the host filled in. Both of today's ops also
            // read kArg0..kArg2 (buf, nLights, cpl), but those are fixed ABI vregs this pass never
            // reassigns, so they need no interval.
            out[0] = in.a; out[1] = in.b; out[2] = in.c; out[3] = in.d;
            return 4;
    }
    return 0;
}

// Does this op WRITE its dst? Branches, Label, Spill and the inline ops do not — their dst field is
// a zero the front-end never fills in, and reading it as a definition would give vreg 0 (kArg0, the
// buffer pointer) a spurious live range that the allocator would then try to manage.
/// Whether an op DEFINES its `dst`, which is what gives it a live interval and what makes the
/// rewriter remap it. Takes the whole instruction because `CallScript` answers per call: the
/// statement form writes nothing, the expression form writes its result.
bool writesDst(const IrInst& in) {
    switch (in.op) {
        case IrOp::Label: case IrOp::BranchGe: case IrOp::BranchGeS: case IrOp::BranchNe:
        // A member store writes MEMORY, not a register: its `a` is the value and `imm` the arena
        // offset, so reading its dst as a definition would give vreg 0 a spurious live range.
        case IrOp::StoreCtrl:
        case IrOp::StoreCtrl32:
        // CallScript writes its dst only when the caller WANTED the value (`b != 0`). The flag has
        // to be consulted: a statement call's dst field is 0, which is kArg4's neighbour kArg0, so
        // treating every call as a definition would give that argument a spurious live interval.
        // Reading it as never-defining is the other error, and the expensive one: the rewriter
        // remaps every source but leaves an unmapped dst, so after a compaction the call wrote the
        // pre-compaction register while its consumer read the new one, and the value was lost.
        case IrOp::CallScript: return in.b != 0;
        case IrOp::Spill: case IrOp::Inline: return false;
        default: return true;
    }
}

}  // namespace

bool spillToBudget(IrProgram& ir, const RegBudget& budget, uint8_t& slotsUsed) {
    // The front end already owns slots 0..localSlots-1 for the script's variables, and both live in
    // the ONE frame — so a spill numbered from zero would land on a loop counter. Start above them,
    // and report the total the prologue must reserve.
    slotsUsed = ir.localSlots;
    if (!ir.ops) return false;

    const uint8_t avail = budget.allocatable();
    // The front end's own variables have to fit the frame whether or not anything spills — it hands
    // out slot indices without knowing the target, and a slot the backend cannot address would be
    // encoded as a truncated offset writing over something else. Checked BEFORE the early return
    // below, or a program that needs no spilling skips the check entirely.
    if (ir.localSlots > kMaxLocals || ir.localSlots > budget.slots) return false;

    // Already fits: leave the program byte-identical. A script that never needed the allocator must
    // not pay a renumbering for its existence — and this is the path every shipped script takes.
    if (ir.vregsUsed <= avail) return true;
    if (ir.vregsUsed > kMaxVRegs) return false;

    // The fixed ABI vregs (buf, nLights, cpl, t, ctrls) arrive in machine registers the host chose
    // and every backend indexes them directly, so they can be neither renumbered nor spilled. They
    // plus the reload temps are the floor: below it there is nothing left to allocate WITH, and the
    // honest answer is to refuse rather than emit code that names a register the target lacks.
    // Reserve temps for the widest op this program ACTUALLY contains, counting DISTINCT sources —
    // not for the widest op the IR can express.
    //
    // The reservation is pure overhead for a program that never uses it, and it is subtracted from a
    // register file that on Xtensa is ten deep. Reserving the theoretical maximum of four left
    // 10 - 1 scratch - 5 fixed ABI vregs - 4 = ZERO keepable, so every looped script was refused
    // outright ("codegen failed") — the allocator had nothing to allocate with. Counting what the
    // program needs is both correct and what makes a loop fit at all on the smallest target.
    // Only a FIXED ABI vreg is exempt: those arrive in registers and are never spilled, so an op
    // reading `buf` or `t` needs no temp for it. Everything else may end up in a slot and therefore
    // may need somewhere to land, so count the distinct non-ABI sources of the widest op present.
    //
    // Note the shape this leaves on Xtensa: 10 registers - 1 inline scratch - 5 fixed ABI vregs
    // leaves 4, and a setRGB reads exactly 4 distinct operands — so a looped effect lands on
    // keepable == 0 and is refused. The pass is right to refuse (it has nothing to allocate with);
    // what is wrong is that FIVE registers are reserved for host arguments that a script reads
    // rarely. Freeing those is register-promotion work, deliberately out of scope for this step.
    uint8_t reloadTemps = 0;
    for (uint16_t i = 0; i < ir.count; i++) {
        VReg s[4];
        const uint8_t n = sourcesOf(ir.ops[i], s);
        uint8_t distinct = 0;
        for (uint8_t a = 0; a < n; a++) {
            if (s[a] < kFirstTemp) continue;                 // a fixed ABI vreg: always a register
            bool seen = false;
            for (uint8_t b = 0; b < a; b++) if (s[b] == s[a]) { seen = true; break; }
            if (!seen) distinct++;
        }
        if (distinct > reloadTemps) reloadTemps = distinct;
    }
    if (reloadTemps > kMaxReloadTemps) reloadTemps = kMaxReloadTemps;
    // The fixed ABI vregs are NOT subtracted any more: core parks them in frame slots at entry, so
    // they hold a register only for the parking store itself — which runs before any temp exists, so
    // sharing those registers afterwards is not a conflict. Reserving five here was holding space for
    // values that had already moved out, and on a ten-register target that was the entire budget.
    if (avail <= reloadTemps) return false;
    const uint8_t keepable = static_cast<uint8_t>(avail - reloadTemps);

    // --- 1. Find the loops, innermost first ----------------------------------------------------
    // Bounded by kIrLabels because a loop needs a label, so this array cannot overflow a program the
    // front-end could build.
    Loop loops[kIrLabels];
    uint8_t loopCount = 0;
    {
        int32_t labelAt[kIrLabels];
        for (auto& p : labelAt) p = -1;
        for (uint16_t i = 0; i < ir.count; i++)
            if (ir.ops[i].op == IrOp::Label && ir.ops[i].imm >= 0 && ir.ops[i].imm < kIrLabels)
                labelAt[ir.ops[i].imm] = i;
        for (uint16_t i = 0; i < ir.count; i++) {
            const IrInst& in = ir.ops[i];
            if (in.op != IrOp::BranchNe) continue;
            if (in.imm < 0 || in.imm >= kIrLabels) return false;      // an unbindable label: refuse
            const int32_t tgt = labelAt[in.imm];
            if (tgt < 0 || static_cast<uint16_t>(tgt) > i) continue;  // forward branch — not a loop
            if (loopCount >= kIrLabels) return false;
            loops[loopCount++] = {static_cast<uint16_t>(tgt), i};
        }
        // Proper nesting is what makes "innermost first" meaningful and what the extension rule below
        // assumes. Two loops must be disjoint or one must contain the other; anything else (which is
        // what a `break` or a `goto` would produce) is refused here rather than allocated against an
        // interval that does not describe the real control flow.
        for (uint8_t x = 0; x < loopCount; x++)
            for (uint8_t y = static_cast<uint8_t>(x + 1); y < loopCount; y++) {
                const bool disjoint = loops[x].back < loops[y].header || loops[y].back < loops[x].header;
                const bool xInY = loops[y].header <= loops[x].header && loops[x].back <= loops[y].back;
                const bool yInX = loops[x].header <= loops[y].header && loops[y].back <= loops[x].back;
                if (!disjoint && !xInY && !yInX) return false;
            }
    }

    // --- 2. Live intervals, then loop extension -------------------------------------------------
    Interval iv[kMaxVRegs];
    for (uint8_t v = 0; v < kMaxVRegs; v++) iv[v].vreg = v;

    auto mention = [&](VReg v, uint16_t at) {
        if (v >= kMaxVRegs) return;
        if (!iv[v].live) { iv[v].live = true; iv[v].start = at; iv[v].end = at; return; }
        if (at < iv[v].start) iv[v].start = at;
        if (at > iv[v].end)   iv[v].end = at;
    };
    for (uint16_t i = 0; i < ir.count; i++) {
        const IrInst& in = ir.ops[i];
        VReg src[4];
        const uint8_t n = sourcesOf(in, src);
        for (uint8_t s = 0; s < n; s++) mention(src[s], i);
        if (writesDst(in)) mention(in.dst, i);
    }

    // A value whose live range TOUCHES a loop is live to that loop's end. Naive first-def-to-last-use
    // is wrong across a back edge: a value defined before the loop and last read early in the body
    // looks dead from the second instruction onward, so the scan would hand its register to something
    // else and the next iteration would read that other value. Extension can only LENGTHEN a range,
    // so its error direction is a needless spill, never a wrong one. Innermost-first, because an
    // extension to an inner loop's end may then have to reach the enclosing loop's end as well.
    for (uint8_t pass = 0; pass < loopCount; pass++) {
        // pick the innermost unprocessed loop = the one containing no other unprocessed loop
        uint8_t pickIdx = 0xff;
        for (uint8_t x = 0; x < loopCount; x++) {
            if (loops[x].header == 0xffff) continue;                  // already processed
            bool containsAnother = false;
            for (uint8_t y = 0; y < loopCount; y++) {
                if (y == x || loops[y].header == 0xffff) continue;
                if (loops[x].header <= loops[y].header && loops[y].back <= loops[x].back)
                    containsAnother = true;
            }
            if (!containsAnother) { pickIdx = x; break; }
        }
        if (pickIdx == 0xff) break;
        const Loop lp = loops[pickIdx];
        loops[pickIdx].header = 0xffff;                               // mark processed
        for (uint8_t v = 0; v < kMaxVRegs; v++)
            if (iv[v].live && iv[v].start <= lp.back && iv[v].end >= lp.header && iv[v].end < lp.back)
                iv[v].end = lp.back;
    }

    // --- 3. Linear scan (Poletto & Sarkar) ------------------------------------------------------
    // Sweep the temps in order of increasing interval start, keeping an `active` set ordered by
    // increasing end. When the set is full, the interval with the FURTHEST end is spilled — it is the
    // one whose register would otherwise be tied up longest, so freeing it buys the most.
    VReg order[kMaxVRegs];
    uint8_t nOrder = 0;
    for (uint8_t v = kFirstTemp; v < ir.vregsUsed; v++) if (iv[v].live) order[nOrder++] = v;
    for (uint8_t i = 1; i < nOrder; i++) {                            // insertion sort: nOrder <= 32
        const VReg k = order[i];
        uint8_t j = i;
        while (j > 0 && iv[order[j - 1]].start > iv[k].start) { order[j] = order[j - 1]; j--; }
        order[j] = k;
    }

    VReg active[kMaxVRegs];
    uint8_t nActive = 0;
    uint8_t nSpilled = ir.localSlots;
    for (uint8_t i = 0; i < nOrder; i++) {
        const VReg cur = order[i];
        // expire: everything whose interval ended before this one starts is free again
        uint8_t w = 0;
        for (uint8_t j = 0; j < nActive; j++)
            if (iv[active[j]].end >= iv[cur].start) active[w++] = active[j];
        nActive = w;

        if (nActive < keepable) {
            active[nActive++] = cur;
            // keep `active` sorted by end so the furthest is always the last element
            for (uint8_t j = nActive - 1; j > 0 && iv[active[j - 1]].end > iv[active[j]].end; j--) {
                const VReg t = active[j]; active[j] = active[j - 1]; active[j - 1] = t;
            }
            continue;
        }
        // nActive >= keepable >= 1 here: the `avail <= reloadTemps` guard above makes keepable at
        // least one, and this branch is only reached when nActive is not below it. Stated because
        // the index below would read active[-1] if that invariant ever moved.
        if (nActive == 0) return false;
        const VReg furthest = active[nActive - 1];
        if (iv[furthest].end > iv[cur].end) {
            iv[furthest].spilled = true;
            iv[furthest].slot = nSpilled++;
            active[nActive - 1] = cur;
            for (uint8_t j = nActive - 1; j > 0 && iv[active[j - 1]].end > iv[active[j]].end; j--) {
                const VReg t = active[j]; active[j] = active[j - 1]; active[j - 1] = t;
            }
        } else {
            iv[cur].spilled = true;
            iv[cur].slot = nSpilled++;
        }
    }
    // Bounded by kMaxLocals, NOT budget.slots: slots kMaxLocals..kTotalSlots-1 hold the parked host
    // arguments (hostArgSlot), which are stored once at entry and reloaded wherever a script reads
    // buf/nLights/cpl/t/ctrls. Allowing a spill into that range would overwrite them — budget.slots
    // is the frame's whole capacity, of which only the bottom kMaxLocals are assignable.
    if (nSpilled > kMaxLocals || nSpilled > budget.slots) return false;

    // --- 4. Compact the survivors ---------------------------------------------------------------
    // The kept temps take the register numbers directly above the fixed ABI vregs, so the rewritten
    // program's high-water mark drops to something the target actually has. The reload temps sit
    // above them, and the backend's own inline scratch above that — a single ascending layout, which
    // is what lets each backend keep computing its scratch from vregsUsed as it already does.
    // Number the kept temps from the BOTTOM of the register file. The ABI vregs keep their own
    // identity for the entry parking store, and that store runs before any temp exists — so an
    // overlap afterwards is not a conflict. Starting at kFirstTemp reserved five registers for
    // values that had already moved to the frame, which pushed the top temps past the budget.
    VReg next = 0;
    for (uint8_t v = kFirstTemp; v < ir.vregsUsed; v++)
        if (iv[v].live && !iv[v].spilled) iv[v].assigned = next++;
    for (uint8_t v = 0; v < kFirstTemp; v++) { iv[v].assigned = v; iv[v].spilled = false; }
    const VReg firstTemp = next;                            // the reload temps start here
    const VReg newHighWater = static_cast<VReg>(firstTemp + reloadTemps);

    // --- 5. Rewrite -----------------------------------------------------------------------------
    // Into a SECOND program: a Reload has to be inserted before the op that reads a spilled value and
    // a Spill after the op that defines one, and a right-sized array has no room to shift into.
    IrProgram out;
    // Worst case per op: four Reloads, the op, one Spill. Over-estimating costs a cold-path
    // allocation; under-estimating would fail a script that fits, so the direction is deliberate.
    const uint32_t want = static_cast<uint32_t>(ir.count) * (kMaxReloadTemps + 2);
    if (want > kMaxIrOps) return false;
    if (!out.reserve(static_cast<uint16_t>(want))) return false;

    auto emit = [&](const IrInst& in) {
        // push() also re-validates every vreg against kMaxVRegs, so a rewrite that named a register
        // outside the budget fails the compile here instead of reaching a backend's register map.
        if (!out.push(in)) return false;
        return true;
    };

    // The function boundaries move with the ops. `fnIrStart` indexes the INPUT array, and this
    // rewrite inserts a Reload before a read and a Spill after a define, so every index past the
    // first insertion shifts. Left unmapped, the lowering closes a function at the wrong op: it
    // emitted a `retw` in the middle of an expanding StoreElem, splitting the pixel write across
    // two frames. Found by disassembling, because the emitted stream was structurally plausible
    // (two entries, two retws, one call8) and only the POSITION of the boundary was wrong.
    //
    // REQUIRED, and the disassembly says otherwise. Removing this makes crosshair.mle emit a
    // TIDIER-looking block (one entry/retw pair per function, at plausible offsets) and every
    // host test still passes, because the host backend cannot reach this path. On an S3 that block
    // boot-loops with StoreProhibited and the buffer pointer holding 0xff: a store through a
    // register the split left holding a color byte. Verify a change here on a board, not on a
    // listing and not on the suite.
    uint16_t newFnStart[kMaxIrEntries] = {};

    for (uint16_t i = 0; i < ir.count; i++) {
        // Record where this function begins in the OUTPUT array, before anything is emitted for
        // the op that starts it.
        for (uint8_t f = 0; f < ir.fnCount; f++)
            if (ir.fnIrStart[f] == i) { newFnStart[f] = out.count; }

        IrInst in = ir.ops[i];
        VReg src[4];
        const uint8_t n = sourcesOf(in, src);

        // Reload each DISTINCT spilled source into its own temp; a repeated operand reuses the temp
        // already holding it, which is both cheaper and necessary — two Reloads of the same slot into
        // different temps would be pure waste on the tightest register file here.
        VReg tempOf[4] = {0, 0, 0, 0};
        uint8_t nTemp = 0;
        for (uint8_t s = 0; s < n; s++) {
            const VReg v = src[s];
            if (v >= kMaxVRegs || !iv[v].spilled) continue;
            bool already = false;
            for (uint8_t p = 0; p < s; p++) if (src[p] == v) { tempOf[s] = tempOf[p]; already = true; break; }
            if (already) continue;
            tempOf[s] = static_cast<VReg>(firstTemp + nTemp);
            nTemp++;
            IrInst rl{};
            rl.op = IrOp::Reload;
            rl.dst = tempOf[s];
            rl.imm = iv[v].slot;
            if (!emit(rl)) return false;
        }

        // Rewrite the operands in place: a spilled one now names its temp, a kept one its compacted
        // number. LoadCtrl's source is kArg4, which is fixed, so it needs no case of its own.
        auto mapped = [&](VReg v, uint8_t slotIdx) -> VReg {
            if (v >= kMaxVRegs) return v;
            return iv[v].spilled ? tempOf[slotIdx] : iv[v].assigned;
        };
        if (n > 0) in.a = mapped(src[0], 0);
        if (n > 1) in.b = mapped(src[1], 1);
        if (n > 2) in.c = mapped(src[2], 2);
        if (n > 3) in.d = mapped(src[3], 3);

        const bool dstSpilled = writesDst(in) && in.dst < kMaxVRegs && iv[in.dst].spilled;
        const uint8_t dstSlot = dstSpilled ? iv[in.dst].slot : 0;
        if (writesDst(in)) {
            // A spilled destination is computed into a reload temp and then stored. It may reuse a
            // temp that carried a source: the op reads its sources and writes its destination as one
            // instruction, so the aliasing is the ordinary `add d, d, b` every ISA here defines.
            in.dst = dstSpilled ? firstTemp : (in.dst < kMaxVRegs ? iv[in.dst].assigned : in.dst);
        }
        if (!emit(in)) return false;
        if (dstSpilled) {
            IrInst sp{};
            sp.op = IrOp::Spill;
            sp.a = firstTemp;
            sp.imm = dstSlot;
            if (!emit(sp)) return false;
        }
    }

    out.vregsUsed = newHighWater;
    // Carry the function table across the swap, with the boundaries remapped to the output array.
    out.fnCount = ir.fnCount;
    for (uint8_t f = 0; f < ir.fnCount; f++) out.fnIrStart[f] = newFnStart[f];
    ir.swap(out);
    slotsUsed = nSpilled;
    return true;
}

}  // namespace mm::moonlive
