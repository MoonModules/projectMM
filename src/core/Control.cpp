// Why .h + .cpp (Control is now in the core-services file-shape list, see
// docs/coding-standards.md § File shape): Control.h started as declarations
// + inline scalar helpers; the JSON serialization / parsing logic grew to
// six switches across three files (HttpServerModule, FilesystemModule,
// scenario_runner). Centralising them here keeps Control.h light for the
// 20+ MoonModule headers that include it just to call addX() and makes
// "add a new ControlType" a single-place edit instead of a hunt across
// three consumers — the "per-type behaviour lives with the type" rule in
// docs/coding-standards.md applied to wire-format serialization.

#include "core/Control.h"

#include "core/JsonSink.h"
#include "core/JsonUtil.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace mm {

const char* controlTypeName(ControlType t) {
    switch (t) {
        case ControlType::Uint8:       return "uint8";
        case ControlType::Uint16:      return "uint16";
        case ControlType::Int16:       return "int16";
        case ControlType::Bool:        return "bool";
        case ControlType::Text:        return "text";
        case ControlType::Password:    return "password";
        case ControlType::ReadOnly:    return "display";
        case ControlType::ReadOnlyInt: return "display-int";
        case ControlType::Select:      return "select";
        case ControlType::Progress:    return "progress";
        case ControlType::IPv4:        return "ipv4";
    }
    return "unknown";
}

bool isPersistable(ControlType t) {
    // Display-only / device-derived types: no point saving — the next
    // loop1s overwrites them.
    switch (t) {
        case ControlType::ReadOnly:
        case ControlType::ReadOnlyInt:
        case ControlType::Progress:
            return false;
        default:
            return true;
    }
}

bool hasDefault(ControlType t) {
    // Defaults are emitted in /api/types so the UI can render a reset-to-
    // default ↺ button. Password is excluded (a default would defeat the
    // secret); the non-persistable types are also excluded (no user
    // input shape to seed).
    if (!isPersistable(t)) return false;
    return t != ControlType::Password;
}

void writeControlValue(JsonSink& sink, const ControlDescriptor& c) {
    switch (c.type) {
        case ControlType::Uint8:
            sink.appendf("%u", *static_cast<uint8_t*>(c.ptr));
            return;
        case ControlType::Uint16:
            sink.appendf("%u", *static_cast<uint16_t*>(c.ptr));
            return;
        case ControlType::Int16:
            sink.appendf("%d", *static_cast<int16_t*>(c.ptr));
            return;
        case ControlType::Bool:
            sink.append(*static_cast<bool*>(c.ptr) ? "true" : "false");
            return;
        case ControlType::Text:
        case ControlType::Password:
        case ControlType::ReadOnly:
            // All three are char-buffer-backed. Password is rendered as a
            // plain JSON string here; the HTTP API obfuscates separately
            // at the writeControls call site (persistence writes plaintext).
            // writeJsonString walks the source straight into the sink with
            // no intermediate fixed buffer, so there's no truncation
            // ceiling regardless of the source buffer's length.
            sink.writeJsonString(static_cast<char*>(c.ptr));
            return;
        case ControlType::ReadOnlyInt:
            sink.appendf("%d", *static_cast<int8_t*>(c.ptr));
            return;
        case ControlType::Select:
            // The selected index — the option strings go in the metadata
            // block (writeControlMetadata) where the UI also wants them.
            sink.appendf("%u", *static_cast<uint8_t*>(c.ptr));
            return;
        case ControlType::Progress:
            sink.appendf("%lu",
                         static_cast<unsigned long>(*static_cast<uint32_t*>(c.ptr)));
            return;
        case ControlType::IPv4: {
            char ipStr[16];
            formatDottedQuad(ipStr, static_cast<const uint8_t*>(c.ptr));
            sink.appendf("\"%s\"", ipStr);
            return;
        }
    }
}

void writeControlMetadata(JsonSink& sink, const ControlDescriptor& c) {
    switch (c.type) {
        case ControlType::Uint8:
        case ControlType::Uint16:
        case ControlType::Int16:
            // All three numeric sliders carry a real [min,max] the UI renders
            // as a range; %d on the int32 fields covers the full uint16 span.
            sink.appendf(",\"min\":%d,\"max\":%d", static_cast<int>(c.min),
                         static_cast<int>(c.max));
            return;
        case ControlType::ReadOnlyInt: {
            // aux holds a borrowed const char* unit suffix (set via
            // addReadOnlyInt). The UI renders "<value> <unit>" verbatim.
            const char* unit = reinterpret_cast<const char*>(c.aux);
            sink.appendf(",\"unit\":\"%s\"", unit ? unit : "");
            return;
        }
        case ControlType::Select: {
            sink.append(",\"options\":[");
            auto* options = reinterpret_cast<const char* const*>(c.aux);
            for (uint8_t o = 0; o < c.max; o++) {
                sink.appendf("%s\"%s\"", o > 0 ? "," : "", options[o]);
            }
            sink.append("]");
            return;
        }
        case ControlType::Progress:
            sink.appendf(",\"total\":%lu", static_cast<unsigned long>(c.aux));
            return;
        // Everything else: no extras.
        case ControlType::Bool:
        case ControlType::Text:
        case ControlType::Password:
        case ControlType::ReadOnly:
        case ControlType::IPv4:
            return;
    }
}

ApplyResult applyControlValue(const ControlDescriptor& c,
                              const char* json, const char* key,
                              ApplyPolicy policy) {
    // Absent key → leave the control at its current value. parseInt/parseBool
    // return 0/false for a missing key, indistinguishable from a real 0, so
    // applying them would clobber a control's non-zero default (e.g. eth
    // phyType=2) when an older/partial persisted file omits the key. The string
    // types already no-op on absence (parseString returns early), but the
    // numeric/select/bool types need this explicit guard. Skipping is correct for
    // both callers: the persistence overlay should preserve defaults for keys it
    // didn't save, and an HTTP /api/control write always includes the key it sets.
    if (!mm::json::hasKey(json, key)) return ApplyResult::Ok;

    // Helper: clamp `v` into [lo, hi] and write to `*dst` of type T.
    // Always returns Ok (clamping is the action, not a failure).
    auto clampInto = [](auto* dst, int v, int lo, int hi) {
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        using T = typename std::remove_pointer<decltype(dst)>::type;
        *dst = static_cast<T>(v);
        return ApplyResult::Ok;
    };
    switch (c.type) {
        case ControlType::Uint8: {
            int v = mm::json::parseInt(json, key);
            // Strict: out-of-range fails. Clamp: snap into [min, max].
            if (policy == ApplyPolicy::Strict && (v < c.min || v > c.max)) {
                return ApplyResult::OutOfRange;
            }
            return clampInto(static_cast<uint8_t*>(c.ptr), v, c.min, c.max);
        }
        case ControlType::Uint16: {
            int v = mm::json::parseInt(json, key);
            // Strict: out-of-[min,max] fails. Clamp: snap into [min,max]. The
            // descriptor's int32 min/max now carry a real uint16 range (default
            // 0..UINT16_MAX = no constraint), so this matches Uint8/Int16.
            if (policy == ApplyPolicy::Strict && (v < c.min || v > c.max)) {
                return ApplyResult::OutOfRange;
            }
            return clampInto(static_cast<uint16_t*>(c.ptr), v, c.min, c.max);
        }
        case ControlType::Int16: {
            int v = mm::json::parseInt(json, key);
            // Strict: out-of-c.min/max fails. Clamp: snap into [min, max].
            // Either way the type-range clamp prevents narrowing wrap
            // (40000 → -25536).
            if (policy == ApplyPolicy::Strict && (v < c.min || v > c.max)) {
                return ApplyResult::OutOfRange;
            }
            return clampInto(static_cast<int16_t*>(c.ptr), v, c.min, c.max);
        }
        case ControlType::Bool:
            *static_cast<bool*>(c.ptr) = mm::json::parseBool(json, key);
            return ApplyResult::Ok;
        case ControlType::Text:
        case ControlType::Password: {
            // Password parses identically to Text — only serialization differs.
            // c.max is the buffer size; parseString writes up to maxLen-1 then
            // NUL-terminates, so passing c.max gives "fill the buffer".
            uint8_t maxLen = static_cast<uint8_t>(c.max > 0 ? c.max : 16);
            mm::json::parseString(json, key, static_cast<char*>(c.ptr), maxLen);
            return ApplyResult::Ok;
        }
        case ControlType::Select: {
            int v = mm::json::parseInt(json, key);
            const int hi = c.max > 0 ? c.max - 1 : 0;
            if (policy == ApplyPolicy::Strict && (v < 0 || v > hi)) {
                return ApplyResult::OutOfRange;
            }
            return clampInto(static_cast<uint8_t*>(c.ptr), v, 0, hi);
        }
        case ControlType::IPv4: {
            char buf[16] = {};
            mm::json::parseString(json, key, buf, sizeof(buf));
            uint8_t octets[4] = {};
            if (!parseDottedQuad(buf, octets)) return ApplyResult::Malformed;
            std::memcpy(c.ptr, octets, 4);
            return ApplyResult::Ok;
        }
        case ControlType::ReadOnly:
        case ControlType::ReadOnlyInt:
        case ControlType::Progress:
            return ApplyResult::ReadOnly;
    }
    return ApplyResult::Malformed;  // unreachable; quiets -Wreturn-type
}

} // namespace mm
