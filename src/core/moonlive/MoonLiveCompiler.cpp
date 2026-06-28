#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"

#include <cstring>   // std::strncmp (@control keyword match)

namespace mm::moonlive {

namespace {

// --- Lexer ---------------------------------------------------------------------------
// `ControlAnno` is a captured `// @control min..max` comment (a control's UI range). A plain
// `//` line comment is skipped like whitespace; only the @control form becomes a token, carrying
// its min/max in annoMin/annoMax. `Assign` is `=` (a control declaration's initializer).
enum class Tok { Ident, Number, Assign, LParen, RParen, Comma, Semicolon, ControlAnno, End, Error };

struct Lexer {
    const char* p;
    Tok kind = Tok::Error;
    long number = 0;
    const char* identBeg = nullptr;
    size_t identLen = 0;
    long annoMin = 0, annoMax = 0;     // ControlAnno: the captured min..max
    const char* tokBeg = nullptr;
    const char* srcBeg;
    const char* err = "";

    explicit Lexer(const char* s) : p(s), srcBeg(s) { advance(); }

    static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    static bool isDigit(char c) { return c >= '0' && c <= '9'; }
    static bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool isIdentCont(char c) { return isIdentStart(c) || isDigit(c); }
    uint16_t col() const { return static_cast<uint16_t>((tokBeg - srcBeg) + 1); }

    // Read a run of digits into v (capped); returns true if at least one digit was consumed.
    bool readNumber(long& v) {
        if (!isDigit(*p)) return false;
        v = 0;
        while (isDigit(*p)) { v = v * 10 + (*p - '0'); p++; if (v > 1000000) break; }
        return true;
    }

    void advance() {
        for (;;) {
            while (isSpace(*p)) p++;
            // Line comment: a plain `//…` is skipped; a `// @control min..max` is captured.
            if (p[0] == '/' && p[1] == '/') {
                const char* lineStart = p;
                p += 2;
                while (*p == ' ' || *p == '\t') p++;
                if (p[0] == '@' && std::strncmp(p, "@control", 8) == 0) {
                    tokBeg = lineStart;
                    p += 8;
                    while (*p == ' ' || *p == '\t') p++;
                    long lo = 0, hi = 0;
                    if (!readNumber(lo) || !(p[0] == '.' && p[1] == '.')) { kind = Tok::Error; err = "malformed @control (expected min..max)"; return; }
                    p += 2;
                    if (!readNumber(hi)) { kind = Tok::Error; err = "malformed @control (expected max)"; return; }
                    annoMin = lo; annoMax = hi; kind = Tok::ControlAnno; return;
                }
                // plain comment — skip to end of line and re-loop (treated as whitespace)
                while (*p && *p != '\n') p++;
                continue;
            }
            break;
        }
        tokBeg = p;
        char c = *p;
        if (c == 0) { kind = Tok::End; return; }
        if (c == '=') { p++; kind = Tok::Assign; return; }
        if (c == '(') { p++; kind = Tok::LParen; return; }
        if (c == ')') { p++; kind = Tok::RParen; return; }
        if (c == ',') { p++; kind = Tok::Comma; return; }
        if (c == ';') { p++; kind = Tok::Semicolon; return; }
        if (isDigit(c)) {
            long v = 0; readNumber(v);
            number = v; kind = Tok::Number; return;
        }
        if (isIdentStart(c)) {
            identBeg = p;
            while (isIdentCont(*p)) p++;
            identLen = static_cast<size_t>(p - identBeg);
            kind = Tok::Ident; return;
        }
        kind = Tok::Error; err = "unexpected character";
    }
};

// --- Parser → IR ---------------------------------------------------------------------
// A recursive-descent parser that evaluates each expression into a virtual register and emits
// IR. It knows the GRAMMAR and resolves call names against the injected table; it owns no
// function names. Buffer writers (Kind::Inline) and helpers (Kind::Call) are dispatched
// generically.
struct Parser {
    Lexer&             lex;
    const BuiltinTable& table;
    IrProgram&         ir;
    VReg               nextTemp = kFirstTemp;   // high-water mark — also IrProgram.vregsUsed
    VReg               freeStack[kMaxVRegs] = {};   // recycled temps (LIFO), so a dead vreg is reused
    uint8_t            freeCount = 0;
    DeclaredControl    controls[kMaxCtrls] = {};  // controls the script declared (decl lines)
    uint8_t            controlCount = 0;
    const char*        error = "";
    uint16_t           errorCol = 0;
    bool               failed = false;

    void fail(const char* msg) { if (!failed) { failed = true; error = msg; errorCol = lex.col(); } }

    // Find a declared control by name; returns its index or -1. Names point into the source buffer
    // (token spans, not NUL-terminated), so compare by length + bytes.
    int findControl(const char* name, size_t len) const {
        for (uint8_t i = 0; i < controlCount; i++)
            if (controls[i].nameLen == len && std::strncmp(controls[i].name, name, len) == 0)
                return i;
        return -1;
    }

    // A stack temp allocator: alloc() hands out a recycled vreg if one is free, else a fresh one;
    // free() returns a temp to the pool once its value has been consumed. This is what keeps a
    // multi-call statement (e.g. setRGB(random16(..), random16(..), random16(..), 0)) within the
    // small device register file — each call's argument temp dies and is reused for the next,
    // instead of the count growing without bound. The textbook tree-walk register stack.
    VReg alloc() {
        if (freeCount) return freeStack[--freeCount];
        if (nextTemp < kMaxVRegs) return nextTemp++;
        // Out of virtual registers — a statement deeper than the fixed file holds. Fail the
        // compile rather than aliasing the last vreg (which would silently produce wrong IR).
        fail("script too complex (out of registers)");
        return kFirstTemp;
    }
    void freeTemp(VReg v) { if (v >= kFirstTemp && freeCount < kMaxVRegs) freeStack[freeCount++] = v; }

    // Append an IR op, failing the compile if the program is full or names an out-of-budget
    // vreg (IrProgram::push validates both). Centralises the check so no call site forgets it.
    void emit(const IrInst& i) { if (!ir.push(i)) fail("script too large"); }

    bool expect(Tok t, const char* msg) {
        if (lex.kind != t) { fail(msg); return false; }
        lex.advance();
        return true;
    }

    // expr := number | ident | call.  Returns the vreg holding the value (or 0 on failure). A bare
    // ident that names a declared control reads its live value (a LoadCtrl of its arena offset); an
    // ident followed by `(` is a call.
    VReg parseExpr() {
        if (failed) return 0;
        if (lex.kind == Tok::Number) {
            if (lex.number < 0 || lex.number > 65535) { fail("number out of range (0..65535)"); return 0; }
            VReg v = alloc();
            emit({IrOp::Const, v, 0,0,0,0, static_cast<int32_t>(lex.number), nullptr, {}});
            lex.advance();
            return v;
        }
        if (lex.kind == Tok::Ident) {
            int ci = findControl(lex.identBeg, lex.identLen);
            if (ci >= 0) {                                // a declared control read
                VReg v = alloc();
                emit({IrOp::LoadCtrl, v, 0,0,0,0, controls[ci].offset, nullptr, {}});
                lex.advance();
                return v;
            }
            VReg out = 0;
            parseCall(&out);   // otherwise a call used as an expression must return a value
            return out;
        }
        fail("expected a number, a control name, or a function call");
        return 0;
    }

    // call := ident "(" [expr {"," expr}] ")".  If `resultOut` is non-null the call is used as
    // an expression and must return a value; the result vreg is written there. If null it is a
    // statement (void).
    void parseCall(VReg* resultOut) {
        if (lex.kind != Tok::Ident) { fail("expected a function name"); return; }
        const Builtin* fn = table.find(lex.identBeg, lex.identLen);
        if (!fn) { fail("unknown function"); return; }
        lex.advance();
        if (!expect(Tok::LParen, "expected '(' after the function name")) return;

        // Evaluate each argument expression into a vreg.
        VReg args[4] = {0,0,0,0};
        uint8_t n = 0;
        if (lex.kind != Tok::RParen) {
            while (true) {
                if (n >= fn->argc || n >= 4) { fail("too many arguments"); return; }
                args[n++] = parseExpr();
                if (failed) return;
                if (lex.kind == Tok::Comma) { lex.advance(); continue; }
                break;
            }
        }
        if (n != fn->argc) { fail("wrong number of arguments"); return; }
        if (!expect(Tok::RParen, "expected ')'")) return;

        // The IR Call op carries a single argument vreg, so a Call-kind builtin must be unary.
        // (Today random16 is the only one.) Reject a multi-arg Call up front rather than silently
        // dropping args[1..]; a future N-ary helper needs the IR Call contract widened first.
        if (fn->kind == BuiltinKind::Call && fn->argc > 1) { fail("multi-argument calls are not supported"); return; }

        if (resultOut) {
            if (fn->kind != BuiltinKind::Call || !fn->returns) { fail("this function does not return a value"); return; }
            // The argument temps are consumed by the call; free them, then allocate the result
            // (which may reuse one of them) — this is what bounds the register count across a
            // chain of calls. The IR Call reads its arg before the result is written, so reuse is
            // safe even when result == arg.
            for (uint8_t i = 0; i < n; i++) freeTemp(args[i]);
            VReg r = alloc();
            emit({IrOp::Call, r, args[0], 0,0,0, 0, fn->fn, {}});
            *resultOut = r;
        } else {
            // A statement call. Call kinds with a result are also allowed as statements (result
            // discarded); Inline kinds emit the inline op with their operands.
            if (fn->kind == BuiltinKind::Call) {
                for (uint8_t i = 0; i < n; i++) freeTemp(args[i]);
                VReg r = alloc();
                emit({IrOp::Call, r, args[0], 0,0,0, 0, fn->fn, {}});
                freeTemp(r);
            } else {
                // Inline op: hand the operand vregs to the backend via an Inline IR op. The
                // operand mapping per inline op is the backend's contract (documented there).
                emit({IrOp::Inline, 0, args[0], args[1], args[2], args[3], 0, nullptr, fn->inlineOp});
                for (uint8_t i = 0; i < n; i++) freeTemp(args[i]);
            }
        }
    }

    // A control declaration: `uint8_t ident = number ;` optionally followed by `// @control min..max`.
    // The leading `uint8_t` keyword is already consumed by the caller. Records a DeclaredControl.
    void parseDecl() {
        if (lex.kind != Tok::Ident) { fail("expected a control name after the type"); return; }
        const char* name = lex.identBeg; size_t nameLen = lex.identLen;
        if (findControl(name, nameLen) >= 0) { fail("duplicate control name"); return; }
        if (controlCount >= kMaxCtrls) { fail("too many controls"); return; }
        lex.advance();
        if (!expect(Tok::Assign, "expected '=' in a control declaration")) return;
        if (lex.kind != Tok::Number) { fail("expected a default value (a number)"); return; }
        if (lex.number < 0 || lex.number > 255) { fail("uint8_t default out of range (0..255)"); return; }
        long def = lex.number;
        lex.advance();
        if (!expect(Tok::Semicolon, "expected ';' after the control declaration")) return;
        // Optional range annotation; default 0..255 if absent.
        long lo = 0, hi = 255;
        if (lex.kind == Tok::ControlAnno) {
            lo = lex.annoMin; hi = lex.annoMax;
            if (lo < 0 || hi > 255 || lo > hi) { fail("@control range out of order or out of 0..255"); return; }
            lex.advance();
        }
        controls[controlCount] = {name, static_cast<uint8_t>(nameLen), CtrlType::Uint8,
                                  static_cast<int32_t>(lo), static_cast<int32_t>(hi),
                                  static_cast<int32_t>(def), controlCount};
        controlCount++;
    }

    // Is the current Ident the `uint8_t` type keyword (the only declared type in Stage 1)?
    bool atTypeKeyword() const {
        return lex.kind == Tok::Ident && lex.identLen == 7 && std::strncmp(lex.identBeg, "uint8_t", 7) == 0;
    }

    // program := { decl } { stmt }.  Declarations (control vars) come first, then one-or-more
    // call statements. (Multi-statement now: a script has decl lines AND a statement line.)
    bool parseProgram() {
        while (!failed && atTypeKeyword()) { lex.advance(); parseDecl(); }
        if (failed) return false;
        if (lex.kind == Tok::End) { fail("empty program (no statement)"); return false; }
        bool any = false;
        while (!failed && lex.kind != Tok::End) {
            if (lex.kind != Tok::Ident) { fail("expected a function call"); return false; }
            parseCall(nullptr);                          // a statement
            if (failed) return false;
            if (!expect(Tok::Semicolon, "expected ';'")) return false;
            any = true;
        }
        if (!any) { fail("expected a statement"); return false; }
        return true;
    }
};

}  // namespace

CompileResult compileSource(const char* source, const BuiltinTable& table, uint8_t* out, size_t cap) {
    CompileResult r;
    if (!source) { r.error = "no source"; return r; }
    if (!out || cap == 0) { r.error = "no code buffer"; return r; }

    Lexer lex(source);
    IrProgram ir;
    Parser parser{lex, table, ir};
    if (!parser.parseProgram()) { r.error = parser.error; r.errorCol = parser.errorCol; return r; }

    size_t len = lowerToBytes(ir, out, cap);
    if (len == 0) { r.error = "codegen failed (unsupported on this target, or too large)"; return r; }
    r.ok = true;
    r.len = len;
    // Surface the declared controls so the binding can create real MoonModule controls.
    r.controlCount = parser.controlCount;
    for (uint8_t i = 0; i < parser.controlCount; i++) r.controls[i] = parser.controls[i];
    return r;
}

}  // namespace mm::moonlive
