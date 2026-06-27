#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"

namespace mm::moonlive {

namespace {

// --- Lexer (unchanged shape) ---------------------------------------------------------
enum class Tok { Ident, Number, LParen, RParen, Comma, Semicolon, End, Error };

struct Lexer {
    const char* p;
    Tok kind = Tok::Error;
    long number = 0;
    const char* identBeg = nullptr;
    size_t identLen = 0;
    const char* tokBeg = nullptr;
    const char* srcBeg;
    const char* err = "";

    explicit Lexer(const char* s) : p(s), srcBeg(s) { advance(); }

    static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    static bool isDigit(char c) { return c >= '0' && c <= '9'; }
    static bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool isIdentCont(char c) { return isIdentStart(c) || isDigit(c); }
    uint16_t col() const { return static_cast<uint16_t>((tokBeg - srcBeg) + 1); }

    void advance() {
        while (isSpace(*p)) p++;
        tokBeg = p;
        char c = *p;
        if (c == 0) { kind = Tok::End; return; }
        if (c == '(') { p++; kind = Tok::LParen; return; }
        if (c == ')') { p++; kind = Tok::RParen; return; }
        if (c == ',') { p++; kind = Tok::Comma; return; }
        if (c == ';') { p++; kind = Tok::Semicolon; return; }
        if (isDigit(c)) {
            long v = 0;
            while (isDigit(*p)) { v = v * 10 + (*p - '0'); p++; if (v > 1000000) break; }
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
    const char*        error = "";
    uint16_t           errorCol = 0;
    bool               failed = false;

    void fail(const char* msg) { if (!failed) { failed = true; error = msg; errorCol = lex.col(); } }

    // A stack temp allocator: alloc() hands out a recycled vreg if one is free, else a fresh one;
    // free() returns a temp to the pool once its value has been consumed. This is what keeps a
    // multi-call statement (e.g. setRGB(random16(..), random16(..), random16(..), 0)) within the
    // small device register file — each call's argument temp dies and is reused for the next,
    // instead of the count growing without bound. The textbook tree-walk register stack.
    VReg alloc() {
        if (freeCount) return freeStack[--freeCount];
        return (nextTemp < kMaxVRegs) ? nextTemp++ : VReg(kMaxVRegs - 1);
    }
    void freeTemp(VReg v) { if (v >= kFirstTemp && freeCount < kMaxVRegs) freeStack[freeCount++] = v; }

    bool expect(Tok t, const char* msg) {
        if (lex.kind != t) { fail(msg); return false; }
        lex.advance();
        return true;
    }

    // expr := number | call.  Returns the vreg holding the value (or 0 on failure).
    VReg parseExpr() {
        if (failed) return 0;
        if (lex.kind == Tok::Number) {
            if (lex.number < 0 || lex.number > 65535) { fail("number out of range (0..65535)"); return 0; }
            VReg v = alloc();
            ir.push({IrOp::Const, v, 0,0,0,0, static_cast<int32_t>(lex.number), nullptr, {}});
            lex.advance();
            return v;
        }
        if (lex.kind == Tok::Ident) {
            VReg out = 0;
            parseCall(&out);   // a call used as an expression must return a value
            return out;
        }
        fail("expected a number or a function call");
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

        if (resultOut) {
            if (fn->kind != BuiltinKind::Call || !fn->returns) { fail("this function does not return a value"); return; }
            // The argument temps are consumed by the call; free them, then allocate the result
            // (which may reuse one of them) — this is what bounds the register count across a
            // chain of calls. The IR Call reads its arg before the result is written, so reuse is
            // safe even when result == arg.
            for (uint8_t i = 0; i < n; i++) freeTemp(args[i]);
            VReg r = alloc();
            ir.push({IrOp::Call, r, args[0], 0,0,0, 0, fn->fn, {}});
            *resultOut = r;
        } else {
            // A statement call. Call kinds with a result are also allowed as statements (result
            // discarded); Inline kinds emit the inline op with their operands.
            if (fn->kind == BuiltinKind::Call) {
                for (uint8_t i = 0; i < n; i++) freeTemp(args[i]);
                VReg r = alloc();
                ir.push({IrOp::Call, r, args[0], 0,0,0, 0, fn->fn, {}});
                freeTemp(r);
            } else {
                // Inline op: hand the operand vregs to the backend via an Inline IR op. The
                // operand mapping per inline op is the backend's contract (documented there).
                ir.push({IrOp::Inline, 0, args[0], args[1], args[2], args[3], 0, nullptr, fn->inlineOp});
                for (uint8_t i = 0; i < n; i++) freeTemp(args[i]);
            }
        }
    }

    bool parseProgram() {
        if (lex.kind != Tok::Ident) {
            fail(lex.kind == Tok::End ? "empty program" : "expected a function call");
            return false;
        }
        parseCall(nullptr);                              // a statement
        if (failed) return false;
        if (!expect(Tok::Semicolon, "expected ';'")) return false;
        if (lex.kind != Tok::End) { fail("unexpected tokens after the statement"); return false; }
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
    return r;
}

}  // namespace mm::moonlive
