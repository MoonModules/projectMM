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
    const char* end = std::strchr(start, '"');
    if (!end) return;
    size_t len = static_cast<size_t>(end - start);
    if (len >= maxLen) len = maxLen - 1;
    std::memcpy(out, start, len);
    out[len] = 0;
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
