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
    return std::strncmp(val, "true", 4) == 0;
}

} // namespace mm::json
