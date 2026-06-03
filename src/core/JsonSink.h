#pragma once

#include "platform/platform.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

// Writes JSON with no fixed-buffer size ceiling. Three modes:
//  - socket mode: a small staging buffer flushes to a TcpConnection as it fills,
//    so the whole response never lives in RAM at once (used by GET /api/state).
//  - buffer mode: bytes collect in a heap buffer that doubles on demand, for
//    callers that need the assembled JSON + its length (the WebSocket push,
//    whose frame header carries the length up front).
//  - fixed-buffer mode: bytes write into a caller-owned slice; the sink
//    flips an overflow flag rather than truncating silently or growing.
//    Used by FilesystemModule's save path, which already owns a sized config
//    buffer and needs the same JSON shape as the live API but with its own
//    overflow-returns-false contract.
// Either way, a module tree of any size serializes correctly.
class JsonSink {
public:
    // Socket mode.
    explicit JsonSink(platform::TcpConnection& conn) : conn_(&conn) {}

    // Buffer mode — collects into a growable heap buffer.
    JsonSink() = default;

    // Fixed-buffer mode — writes into a caller-owned slice (no heap, no
    // truncation: overflow sets a flag the caller checks via overflowed()).
    // `cap` is the total writable capacity; the sink reserves one byte for
    // a trailing NUL so `data()` stays C-string-safe.
    JsonSink(char* buf, size_t cap) : fixed_(buf), fixedCap_(cap) {
        if (fixed_ && fixedCap_ > 0) fixed_[0] = '\0';
    }

    ~JsonSink() { if (heap_) platform::free(heap_); }

    JsonSink(const JsonSink&) = delete;
    JsonSink& operator=(const JsonSink&) = delete;

    void append(const char* s) {
        if (!s) return;
        while (*s) {
            if (conn_) {
                if (pos_ == STAGE_SIZE) flushStage();
                stage_[pos_++] = *s++;
            } else if (fixed_) {
                // +1 reserves the NUL slot. Overflow stops writing; once
                // tripped, subsequent appends are no-ops so the caller sees
                // a consistent overflowed() state.
                if (fixedLen_ + 1 >= fixedCap_) { overflowed_ = true; return; }
                fixed_[fixedLen_++] = *s++;
                fixed_[fixedLen_] = '\0';
            } else {
                if (!ensureHeap(heapLen_ + 1)) return;  // out of memory: drop
                heap_[heapLen_++] = *s++;
            }
        }
        // Keep heap_ null-terminated after every append so data() is a valid
        // C-string even if the caller skips size(). ensureHeap already reserved
        // the +1 slot, so this write is in-bounds.
        if (!conn_ && !fixed_ && heap_) heap_[heapLen_] = '\0';
    }

    // Append a printf-formatted fragment. The common case (one control, one
    // module header) fits the FRAG_MAX stack buffer; a fragment that would
    // exceed it — e.g. an unusually long text-control value — is re-formatted
    // into a heap buffer so the output is never silently truncated.
    void appendf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char frag[FRAG_MAX];
        va_list ap;
        va_start(ap, fmt);
        va_list ap2;
        va_copy(ap2, ap);
        int n = std::vsnprintf(frag, sizeof(frag), fmt, ap);
        va_end(ap);
        if (n < 0) { va_end(ap2); return; }
        if (static_cast<size_t>(n) < sizeof(frag)) {
            va_end(ap2);
            append(frag);
            return;
        }
        // Fragment longer than the stack buffer.
        if (fixed_) {
            // Fixed-buffer mode never heap-allocs — capacity is bounded by
            // the caller's slice on purpose, and an alloc here would silently
            // succeed even when the slice is too small for the formatted
            // fragment. Flag overflow so the caller (e.g. FilesystemModule's
            // writeValue) aborts the file/response.
            overflowed_ = true;
            va_end(ap2);
            return;
        }
        // Socket / heap-grow modes: format into an exact heap buffer so the
        // long fragment isn't silently truncated.
        char* big = static_cast<char*>(platform::alloc(static_cast<size_t>(n) + 1));
        if (big) {
            std::vsnprintf(big, static_cast<size_t>(n) + 1, fmt, ap2);
            append(big);
            platform::free(big);
        }
        va_end(ap2);
    }

    // Generic value-fragment writers — produce a syntactically correct JSON
    // value (number / bool / string) into the sink. Used by the ControlType
    // serializers (Control.cpp) and by anyone bridging a typed value into
    // wire-format text (scenario_runner's JsonVal → applyControlValue path).
    // Accepts `double` because the only caller is `scenario_runner.cpp`'s
    // JsonVal → JSON-text bridge — JsonVal stores numerics as `double` so
    // it doesn't lose precision parsing scenario fixtures. The runner is
    // desktop / test-only.
    //
    // **Production firmware must not call this** — see docs/coding-standards.md
    // § Prefer integers: `double` runs in software emulation on ESP32 Xtensa
    // (~30x slower than `float`). Production code paths use the typed
    // serializers in `Control.cpp` (writeControlValue) which dispatch on
    // ControlType and emit integer / bool / string fragments without going
    // near `double`.
    void writeNumber(double v) {
        // Integer-valued doubles render as ints (avoids "42.0000" noise);
        // genuine fractionals use %g for compact display.
        if (v == static_cast<double>(static_cast<long long>(v))) {
            appendf("%lld", static_cast<long long>(v));
        } else {
            appendf("%g", v);
        }
    }
    void writeBool(bool v) { append(v ? "true" : "false"); }
    void writeJsonString(const char* s) {
        // Walks the source char-by-char straight into the sink — no
        // intermediate fixed buffer, so there's no truncation ceiling
        // regardless of input length. Each per-char append is a small
        // write the sink's per-mode logic handles uniformly (socket flush
        // / heap grow / fixed-buffer overflow flag).
        //
        // RFC 8259 §7: strings MUST escape `"`, `\`, and any byte < 0x20.
        // Bare control bytes in JSON are a parser error. Named escapes
        // for the common ones (\n / \r / \t / \b / \f), `\u00XX` for the
        // rest. Bytes ≥ 0x20 (including UTF-8 continuation bytes) pass
        // through unmodified — the receiver decodes UTF-8 itself.
        if (!s) s = "";
        append("\"");
        char buf[8];  // longest emission is "\uXXXX" (6) + NUL; 8 is round
        for (; *s; s++) {
            unsigned char c = static_cast<unsigned char>(*s);
            if (c == '"' || c == '\\') {
                buf[0] = '\\'; buf[1] = static_cast<char>(c); buf[2] = 0;
                append(buf);
            } else if (c == '\n') { append("\\n"); }
            else if (c == '\r') { append("\\r"); }
            else if (c == '\t') { append("\\t"); }
            else if (c == '\b') { append("\\b"); }
            else if (c == '\f') { append("\\f"); }
            else if (c < 0x20) {
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                append(buf);
            } else {
                buf[0] = static_cast<char>(c); buf[1] = 0;
                append(buf);
            }
        }
        append("\"");
    }

    // Socket mode: flush staged bytes to the socket. Call once at the end.
    void flush() { flushStage(); }

    // Buffer mode: the collected JSON and its length (null-terminated).
    // Fixed-buffer mode: same — data() points at the caller's buffer,
    // size() is the bytes written so far.
    const char* data() const {
        if (fixed_) return fixed_;
        return heap_ ? heap_ : "";
    }
    size_t size() const { return fixed_ ? fixedLen_ : heapLen_; }

    // Fixed-buffer mode only: did any append run out of capacity?
    bool overflowed() const { return overflowed_; }

private:
    static constexpr size_t STAGE_SIZE = 1024;
    static constexpr size_t FRAG_MAX = 256;

    void flushStage() {
        if (conn_ && pos_ > 0) {
            conn_->write(reinterpret_cast<const uint8_t*>(stage_), pos_);
            pos_ = 0;
        }
    }

    // Grow the heap buffer to hold at least `need` bytes plus a null terminator.
    bool ensureHeap(size_t need) {
        if (need + 1 <= heapCap_) return true;
        size_t newCap = heapCap_ == 0 ? 2048 : heapCap_ * 2;
        while (newCap < need + 1) newCap *= 2;
        char* grown = static_cast<char*>(platform::alloc(newCap));
        if (!grown) return false;
        if (heap_) { std::memcpy(grown, heap_, heapLen_); platform::free(heap_); }
        heap_ = grown;
        heapCap_ = newCap;
        heap_[heapLen_] = 0;
        return true;
    }

    platform::TcpConnection* conn_ = nullptr;  // socket mode when non-null
    char stage_[STAGE_SIZE];
    size_t pos_ = 0;

    char* heap_ = nullptr;                     // buffer mode
    size_t heapLen_ = 0;
    size_t heapCap_ = 0;

    char* fixed_ = nullptr;                    // fixed-buffer mode
    size_t fixedLen_ = 0;
    size_t fixedCap_ = 0;
    bool overflowed_ = false;
};

// Escape a string for embedding inside a JSON string literal: " → \" and
// \ → \\. Writes into `out` (no surrounding quotes). Truncates rather than
// overflowing if the escaped form exceeds outMax. Distinct from
// FilesystemModule::writeJsonString, which appends to a (buf, pos) pair.
inline void jsonEscape(const char* in, char* out, size_t outMax) {
    if (outMax == 0) return;  // no room even for the terminator
    size_t oi = 0;
    for (; *in && oi + 2 < outMax; in++) {
        if (*in == '"' || *in == '\\') out[oi++] = '\\';
        out[oi++] = *in;
    }
    out[oi] = 0;
}

} // namespace mm
