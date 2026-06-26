#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/moonlive_emit.h"

namespace mm::moonlive {

namespace {

// --- Lexer ---------------------------------------------------------------------------
// The token kinds the grammar needs. A hand-written recursive-descent front-end (the textbook
// embedded-script default) lexes on demand: the parser pulls one token at a time, and
// advance() walks the source directly without building a token list.
enum class Tok { Ident, Number, LParen, RParen, Comma, Semicolon, End, Error };

struct Lexer {
    const char* p;            // cursor into the source
    Tok kind = Tok::Error;
    long number = 0;          // value when kind==Number
    const char* identBeg = nullptr;
    size_t identLen = 0;      // span when kind==Ident
    const char* tokBeg = nullptr;   // start of the current token (for error columns)
    const char* srcBeg;       // start of source (for 1-based column math)
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

    bool identIs(const char* word) const {
        if (kind != Tok::Ident) return false;
        for (size_t i = 0; i < identLen; i++) if (identBeg[i] != word[i] || word[i] == 0) return false;
        return word[identLen] == 0;
    }
};

// --- AST + Parser --------------------------------------------------------------------
// The only production: program := "fill" "(" number "," number "," number ")" ";" End.
// The AST for it is just the three colour bytes — a single statement needs no node hierarchy.
struct Parsed {
    bool ok = false;
    const char* error = "";
    uint16_t errorCol = 0;
    uint8_t r = 0, g = 0, b = 0;
};

// Pull a 0..255 number token; fail with a diagnostic otherwise.
bool expectByte(Lexer& lex, uint8_t& out, Parsed& res) {
    if (lex.kind == Tok::Error) { res.error = lex.err; res.errorCol = lex.col(); return false; }
    if (lex.kind != Tok::Number) { res.error = "expected a number"; res.errorCol = lex.col(); return false; }
    if (lex.number < 0 || lex.number > 255) { res.error = "colour value out of range (0..255)"; res.errorCol = lex.col(); return false; }
    out = static_cast<uint8_t>(lex.number);
    lex.advance();
    return true;
}

bool expect(Lexer& lex, Tok t, const char* msg, Parsed& res) {
    if (lex.kind != t) { res.error = msg; res.errorCol = lex.col(); return false; }
    lex.advance();
    return true;
}

Parsed parse(const char* source) {
    Parsed res;
    Lexer lex(source);

    if (!lex.identIs("fill")) {
        res.error = (lex.kind == Tok::End) ? "empty program (expected fill(...))" : "expected 'fill'";
        res.errorCol = lex.col();
        return res;
    }
    lex.advance();
    if (!expect(lex, Tok::LParen, "expected '(' after fill", res)) return res;
    if (!expectByte(lex, res.r, res)) return res;
    if (!expect(lex, Tok::Comma, "expected ',' between colour values", res)) return res;
    if (!expectByte(lex, res.g, res)) return res;
    if (!expect(lex, Tok::Comma, "expected ',' between colour values", res)) return res;
    if (!expectByte(lex, res.b, res)) return res;
    if (!expect(lex, Tok::RParen, "expected ')' to close fill(...)", res)) return res;
    if (!expect(lex, Tok::Semicolon, "expected ';' after fill(...)", res)) return res;
    if (lex.kind != Tok::End) { res.error = "unexpected tokens after fill(...)"; res.errorCol = lex.col(); return res; }

    res.ok = true;
    return res;
}

}  // namespace

CompileResult compileSource(const char* source, uint8_t* out, size_t cap) {
    CompileResult r;
    if (!source) { r.error = "no source"; return r; }
    if (!out || cap == 0) { r.error = "no code buffer"; return r; }   // never emit through a null/empty buffer

    Parsed p = parse(source);
    if (!p.ok) { r.error = p.error; r.errorCol = p.errorCol; return r; }

    // Codegen: the parsed colour drives the SAME per-ISA emitter the typed compile path uses —
    // so a parsed `fill(0,0,255)` produces byte-for-byte the bytes emitFill(0,0,255) does (the
    // golden-bytes equivalence the test asserts).
    size_t len = emitFill(out, cap, p.r, p.g, p.b);
    if (len == 0) { r.error = "code buffer too small"; return r; }
    r.ok = true;
    r.len = len;
    return r;
}

}  // namespace mm::moonlive
