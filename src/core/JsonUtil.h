#pragma once

// JSON helpers for projectMM. Two layers, both header-only, both off the hot path
// (persistence load at boot, control writes) so bounded stack use is fine:
//
//   1. Flat helpers (parseString/hasKey/parseInt/parseBool): first-match key lookup
//      over the subset we emit — flat key/value pairs, optional whitespace after the
//      colon (Python's json.dumps inserts it), string/integer/boolean values. They do
//      not descend into nested objects or arrays; many callers rely on this cheap
//      strstr-based scan (HttpServerModule, FilesystemModule, scenario_runner, Control).
//
//   2. Recursive reader (JsonDoc/parse + the read/get accessors below): a standard
//      recursive-descent parser into a fixed node arena, the recognizable shape for
//      walking nested structure — needed for the persisted device list, an array of
//      small objects. Bounded by design for the ESP32 task stack: a compile-time node
//      pool (kMaxNodes) and a recursion depth guard (kMaxDepth) mean no heap and no
//      unbounded recursion; any malformed, truncated, or oversized input fails cleanly
//      (parse() returns false, accessors return safe defaults) and never reads OOB.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm::json {

inline void parseString(const char* json, const char* key, char* out, size_t maxLen) {
    if (!json || !key || !out || maxLen == 0) return;
    char search[48];
    std::snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char* start = std::strstr(json, search);
    if (!start) {
        std::snprintf(search, sizeof(search), "\"%s\": \"", key);
        start = std::strstr(json, search);
    }
    if (!start) return;
    start += std::strlen(search);
    // Copy until the real closing quote, un-escaping \" and \\. A bare strchr
    // for '"' would stop at an escaped quote inside the value — must honour the
    // backslash escapes written by FilesystemModule::writeJsonString.
    size_t oi = 0;
    for (const char* p = start; *p && oi + 1 < maxLen; p++) {
        if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
            p++;                 // skip the backslash, copy the escaped char
        } else if (*p == '"') {
            break;               // unescaped quote — end of string
        }
        out[oi++] = *p;
    }
    out[oi] = 0;
}

// True when `key` is present in the JSON object. Lets callers distinguish a
// genuinely-absent key from one whose value happens to be 0/false — parseInt and
// parseBool can't, so applying their result for an absent key would clobber a
// control's non-zero default (e.g. eth phyType=2) with 0 on a partial/older save.
inline bool hasKey(const char* json, const char* key) {
    if (!json || !key) return false;
    char search[48];
    std::snprintf(search, sizeof(search), "\"%s\":", key);
    return std::strstr(json, search) != nullptr;
}

inline int parseInt(const char* json, const char* key) {
    if (!json || !key) return 0;
    char search[48];
    std::snprintf(search, sizeof(search), "\"%s\":", key);
    const char* start = std::strstr(json, search);
    if (!start) {
        std::snprintf(search, sizeof(search), "\"%s\": ", key);
        start = std::strstr(json, search);
    }
    if (!start) return 0;
    return std::atoi(start + std::strlen(search));
}

inline bool parseBool(const char* json, const char* key) {
    if (!json || !key) return false;
    char search[48];
    std::snprintf(search, sizeof(search), "\"%s\":", key);
    const char* start = std::strstr(json, search);
    if (!start) {
        std::snprintf(search, sizeof(search), "\"%s\": ", key);
        start = std::strstr(json, search);
    }
    if (!start) return false;
    const char* val = start + std::strlen(search);
    while (*val == ' ') val++;
    // Accept both the JSON literal `true` and a numeric `1` — boards.json / the
    // catalog fan-out historically wrote 0/1 for flags that are now Bool controls
    // (e.g. ethClockExtIn), and some HTTP clients send 1/0; treat either as true.
    return std::strncmp(val, "true", 4) == 0 || *val == '1';
}

// --- Recursive reader -------------------------------------------------------
//
// A standard recursive-descent parser into a fixed node arena. parse() copies the
// input into the document's own buffer (so strings can be NUL-terminated in place,
// un-escaped) and links nodes by index — no pointers into caller memory, no heap.

// kMaxNodes: every value (object, array, string, number, bool, null) is one node, plus
// one node per object member key. 128 covers our actual use: a ~32-element array of
// small objects (the persisted device list — each device is one object node + a few
// field nodes). kMaxDepth bounds recursion for the ESP32 task stack (~3.5-8 KB); 16 is
// far deeper than anything we emit (array -> object -> value is depth 3).
inline constexpr int kMaxNodes = 128;
inline constexpr int kMaxDepth = 16;
inline constexpr int kMaxJsonLen = 4096;  // arena for the copied/un-escaped input text

enum class JsonType : uint8_t { Null, Bool, Int, String, Object, Array };

// One value in the arena. Children form a singly-linked list by index (firstChild ->
// next -> next -> ...), which keeps each node fixed-size with no per-node child array.
// For an object member, `key` points into the doc buffer (the member name); the member's
// value is the node itself.
struct JsonNode {
    JsonType type = JsonType::Null;
    const char* key = nullptr;    // member name when this node is an object member, else nullptr
    const char* str = nullptr;    // string value (points into doc buffer) when type == String
    long intValue = 0;            // numeric value when type == Int; 0/1 mirror for Bool
    int firstChild = -1;          // index of first child node, or -1
    int next = -1;                // index of next sibling, or -1
};

// The parsed document: owns the text buffer and node arena. Returned by reference from
// parse(); the caller keeps it alive while walking. No heap — sized for kMaxJsonLen /
// kMaxNodes, which is ~5 KB; fine for a boot-time persistence load, not the hot path.
struct JsonDoc {
    char buf[kMaxJsonLen];
    JsonNode nodes[kMaxNodes];
    int count = 0;
    int root = -1;

    bool valid() const { return root >= 0; }
    const JsonNode* node(int i) const { return (i >= 0 && i < count) ? &nodes[i] : nullptr; }
    const JsonNode* rootNode() const { return node(root); }
};

namespace detail {

// Parser cursor over the doc's own (mutable) buffer. Un-escaping rewrites string bytes in
// place, so the buffer doubles as scratch — a parsed string is NUL-terminated where its
// closing quote was, and `str` points at its first byte.
struct JsonParser {
    JsonDoc& doc;
    char* p;        // current position in doc.buf
    bool ok = true;

    explicit JsonParser(JsonDoc& d) : doc(d), p(d.buf) {}

    void skipWs() { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; }

    int alloc() {
        if (doc.count >= kMaxNodes) { ok = false; return -1; }
        return doc.count++;
    }

    // Parse a JSON string literal: assumes *p == '"'. Un-escapes in place and NUL-terminates,
    // returning a pointer to the first byte. Handles \" \\ \/ \n \r \t \b \f; \uXXXX is passed
    // through as raw bytes (no decode — we never emit it). Returns nullptr on unterminated input.
    char* parseStringLiteral() {
        p++;                       // opening quote
        char* out = p;             // write cursor (<= read cursor, so in-place is safe)
        char* start = out;
        while (*p) {
            char c = *p++;
            if (c == '"') { *out = 0; return start; }
            if (c == '\\') {
                char e = *p++;
                switch (e) {
                    case '"':  c = '"';  break;
                    case '\\': c = '\\'; break;
                    case '/':  c = '/';  break;
                    case 'n':  c = '\n'; break;
                    case 'r':  c = '\r'; break;
                    case 't':  c = '\t'; break;
                    case 'b':  c = '\b'; break;
                    case 'f':  c = '\f'; break;
                    case 0:    ok = false; return nullptr;  // trailing backslash, truncated
                    default:   c = e;    break;             // unknown escape (incl. \u) — keep byte
                }
            }
            *out++ = c;
        }
        ok = false;                // ran off the end without a closing quote
        return nullptr;
    }

    int parseValue(int depth) {
        if (!ok) return -1;
        if (depth >= kMaxDepth) { ok = false; return -1; }
        skipWs();
        switch (*p) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': {
                int idx = alloc();
                char* s = parseStringLiteral();
                if (!ok || idx < 0) return -1;
                doc.nodes[idx].type = JsonType::String;
                doc.nodes[idx].str = s;
                return idx;
            }
            case 't':
            case 'f': {
                bool isTrue = (*p == 't');
                const char* lit = isTrue ? "true" : "false";
                size_t n = std::strlen(lit);
                if (std::strncmp(p, lit, n) != 0) { ok = false; return -1; }
                p += n;
                int idx = alloc();
                if (idx < 0) return -1;
                doc.nodes[idx].type = JsonType::Bool;
                doc.nodes[idx].intValue = isTrue ? 1 : 0;
                return idx;
            }
            case 'n': {
                if (std::strncmp(p, "null", 4) != 0) { ok = false; return -1; }
                p += 4;
                int idx = alloc();
                if (idx < 0) return -1;
                doc.nodes[idx].type = JsonType::Null;
                return idx;
            }
            default: {
                // Number. Integer-only model: read the optional sign + digits, then
                // discard any fractional/exponent tail (truncate) — we never persist floats.
                if (*p != '-' && (*p < '0' || *p > '9')) { ok = false; return -1; }
                char* endp = nullptr;
                long v = std::strtol(p, &endp, 10);
                if (endp == p) { ok = false; return -1; }
                p = endp;
                // Skip a well-formed fractional/exponent tail (we keep only the integer
                // part — never persist floats). Precise so we stop at the number's real
                // end and don't swallow a following token: `1-2` reads `1` then leaves
                // `-2`, not one run. Fraction: '.' then digits. Exponent: e/E, optional
                // sign, then digits.
                auto digits = [&] { while (*p >= '0' && *p <= '9') p++; };
                if (*p == '.') { p++; digits(); }
                if (*p == 'e' || *p == 'E') {
                    char* e = p++;
                    if (*p == '+' || *p == '-') p++;
                    if (*p >= '0' && *p <= '9') digits();
                    else p = e;     // bare 'e' with no exponent digits — not part of the number
                }
                int idx = alloc();
                if (idx < 0) return -1;
                doc.nodes[idx].type = JsonType::Int;
                doc.nodes[idx].intValue = v;
                return idx;
            }
        }
    }

    int parseObject(int depth) {
        int self = alloc();
        if (self < 0) return -1;
        doc.nodes[self].type = JsonType::Object;
        p++;                       // '{'
        skipWs();
        int prev = -1;
        if (*p == '}') { p++; return self; }
        while (ok) {
            skipWs();
            if (*p != '"') { ok = false; return -1; }
            char* key = parseStringLiteral();
            if (!ok) return -1;
            skipWs();
            if (*p != ':') { ok = false; return -1; }
            p++;
            int child = parseValue(depth + 1);
            if (!ok || child < 0) return -1;
            doc.nodes[child].key = key;
            if (prev < 0) doc.nodes[self].firstChild = child;
            else          doc.nodes[prev].next = child;
            prev = child;
            skipWs();
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; return self; }
            ok = false; return -1;
        }
        return -1;
    }

    int parseArray(int depth) {
        int self = alloc();
        if (self < 0) return -1;
        doc.nodes[self].type = JsonType::Array;
        p++;                       // '['
        skipWs();
        int prev = -1;
        if (*p == ']') { p++; return self; }
        while (ok) {
            int child = parseValue(depth + 1);
            if (!ok || child < 0) return -1;
            if (prev < 0) doc.nodes[self].firstChild = child;
            else          doc.nodes[prev].next = child;
            prev = child;
            skipWs();
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; return self; }
            ok = false; return -1;
        }
        return -1;
    }
};

}  // namespace detail

// Parse `json` into `out`. Returns true on success (out.root is the top value), false on
// any malformed, truncated, oversized, or too-deep input — out is left invalid (root == -1).
// Safe on a null pointer and the empty string. Trailing whitespace is allowed; trailing
// non-whitespace garbage (e.g. "}{][") fails.
inline bool parse(const char* json, JsonDoc& out) {
    out.count = 0;
    out.root = -1;
    if (!json) return false;
    size_t len = std::strlen(json);
    if (len == 0 || len + 1 > sizeof(out.buf)) return false;
    std::memcpy(out.buf, json, len + 1);

    detail::JsonParser parser(out);
    int root = parser.parseValue(0);
    if (!parser.ok || root < 0) return false;
    parser.skipWs();
    if (*parser.p != 0) return false;   // trailing garbage after the top-level value
    out.root = root;
    return true;
}

// --- Navigation -------------------------------------------------------------
// All accessors are null-safe and bounds-safe: pass a node from doc.node(...) (or nullptr),
// get back a child node / safe default. They never crash on a wrong-typed or missing node.

// Member of an object by key, or nullptr if `obj` is not an object / has no such member.
inline const JsonNode* member(const JsonDoc& doc, const JsonNode* obj, const char* key) {
    if (!obj || obj->type != JsonType::Object || !key) return nullptr;
    for (int i = obj->firstChild; i >= 0;) {
        const JsonNode* n = doc.node(i);
        if (!n) break;
        if (n->key && std::strcmp(n->key, key) == 0) return n;
        i = n->next;
    }
    return nullptr;
}

// Number of elements in an array (0 if `arr` is not an array).
inline int arraySize(const JsonDoc& doc, const JsonNode* arr) {
    if (!arr || arr->type != JsonType::Array) return 0;
    int count = 0;
    for (int i = arr->firstChild; i >= 0;) {
        const JsonNode* n = doc.node(i);
        if (!n) break;
        count++;
        i = n->next;
    }
    return count;
}

// Element `index` of an array, or nullptr if out of range / not an array.
inline const JsonNode* element(const JsonDoc& doc, const JsonNode* arr, int index) {
    if (!arr || arr->type != JsonType::Array || index < 0) return nullptr;
    int at = 0;
    for (int i = arr->firstChild; i >= 0;) {
        const JsonNode* n = doc.node(i);
        if (!n) break;
        if (at == index) return n;
        at++;
        i = n->next;
    }
    return nullptr;
}

// Read a node as a string into `out` (always NUL-terminated). Empty string for a non-string
// node or null node. Returns true when a string value was copied.
inline bool readString(const JsonNode* n, char* out, size_t maxLen) {
    if (!out || maxLen == 0) return false;
    out[0] = 0;
    if (!n || n->type != JsonType::String || !n->str) return false;
    std::strncpy(out, n->str, maxLen - 1);
    out[maxLen - 1] = 0;
    return true;
}

// Read a node as an int. A Bool reads as 0/1; anything else returns `fallback`.
inline long readInt(const JsonNode* n, long fallback = 0) {
    if (!n) return fallback;
    if (n->type == JsonType::Int || n->type == JsonType::Bool) return n->intValue;
    return fallback;
}

// Read a node as a bool. A non-zero Int reads as true; anything non-bool/non-int is `fallback`.
inline bool readBool(const JsonNode* n, bool fallback = false) {
    if (!n) return fallback;
    if (n->type == JsonType::Bool || n->type == JsonType::Int) return n->intValue != 0;
    return fallback;
}

// Parse `json`, find the array at `key`, and call `fn(doc, element)` for each OBJECT
// element. This is the boilerplate every persisted-list restore shares — parse,
// navigate, type-check, iterate, malformed-safety — so it lives here in core; a caller
// (a ListSource's restoreList) supplies only the per-element "read my fields" body and
// stays a few plain lines. `fn` is a template callback (zero-overhead, no std::function
// / heap). Non-object elements are skipped. Returns false on malformed/missing/non-array
// (the caller's list is simply not restored). The JsonDoc lives on this call's stack —
// boot-time load, not the hot path. Recognizable callback-iteration shape.
// The non-template core: parse `json` and navigate to the array under `key`, returning
// the array node (or nullptr on malformed/missing/non-array) along with the shared doc.
// Lives in a .cpp-less inline but in its OWN non-template function so the heavy static
// JsonDoc below is ONE copy in .bss regardless of how many callback types instantiate
// forEachListElement — a per-instantiation static would multiply the ~8 KB doc by the
// number of distinct lists (and "we'll add more lists"). The doc is a function-local
// static (not a stack local: ~8 KB overflows the ESP32 task stack → boot-loop) and is
// safe to share because JSON parsing is strictly serial — boot-time load or a single
// control write, never concurrent (same single-owner-buffer reasoning as
// FilesystemModule::fileBuf_). Returns the doc by out-param so the caller can read fields.
inline const JsonNode* parseListArray(const char* json, const char* key, JsonDoc*& docOut) {
    static JsonDoc doc;
    docOut = &doc;
    if (!parse(json, doc)) return nullptr;
    const JsonNode* arr = member(doc, doc.rootNode(), key);
    if (!arr || arr->type != JsonType::Array) return nullptr;
    return arr;
}

template <typename Fn>
inline bool forEachListElement(const char* json, const char* key, Fn&& fn) {
    JsonDoc* doc = nullptr;
    const JsonNode* arr = parseListArray(json, key, doc);
    if (!arr) return false;
    const int n = arraySize(*doc, arr);
    for (int i = 0; i < n; i++) {
        const JsonNode* el = element(*doc, arr, i);
        if (el && el->type == JsonType::Object) fn(*doc, el);
    }
    return true;
}

} // namespace mm::json
