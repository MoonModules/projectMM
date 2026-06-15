#pragma once

// Minimal flat-JSON helpers. Producer and consumer are both inside projectMM, so this
// parser only handles the subset of JSON we emit: flat key/value pairs, optional whitespace
// after colons (Python's json.dumps inserts these), string/integer/boolean values.
//
// NOT recursive — first match wins. Do NOT add nested object/array support here. Plan-09
// grew this header to 256 lines and that was a warning sign — the project does not need a
// general-purpose JSON library. Three functions, ~50 lines, header-only.

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

} // namespace mm::json
