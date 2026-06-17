#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Pure device-identification logic for DevicesModule, factored out so it is
// host-unit-testable without network I/O (the "second caller extracts the helper"
// pattern, cf. PinList.h / Control.cpp's parsers — the unit test is the second
// caller). Everything here is a pure function over a response-body string: no
// sockets, no module state. Tolerant of any input (a hostile / truncated / garbage
// body yields DevType::Generic and an empty name), per the robustness contract for
// network-sourced data — this reads a FOREIGN device's reply, so it stays local and
// defensive rather than using the project's own JsonUtil control-key parser.

namespace mm {

// What a discovered device is, inferred from its HTTP response.
enum class DevType : uint8_t { Generic = 0, ProjectMM = 1, Wled = 2 };

inline const char* devTypeStr(DevType t) {
    switch (t) {
        case DevType::ProjectMM: return "projectMM";
        case DevType::Wled:      return "WLED";
        case DevType::Generic:   return "generic";
    }
    return "generic";
}

// Classify a live host from a probe body. `stateBody` is the /api/state reply (or
// null/empty if that probe didn't 200); `infoBody` is the /json/info reply. A
// projectMM /api/state carries a top-level "modules" array; a WLED /json/info
// carries "WLED"; anything else live is generic.
inline DevType classifyDevice(const char* stateBody, const char* infoBody) {
    if (stateBody && std::strstr(stateBody, "\"modules\"")) return DevType::ProjectMM;
    if (infoBody && std::strstr(infoBody, "WLED")) return DevType::Wled;
    return DevType::Generic;
}

// Minimal one-shot JSON string scan: find `anchor`; if `valueKey` is null, read the
// first "..." after the next colon; if non-null, read the first "..." after the next
// occurrence of `valueKey` (for a value nested in an object, e.g. the "deviceName"
// control's "value"). Writes "" when not found. Bounds-checked; tolerant of garbage.
inline void extractStringAfter(const char* body, const char* anchor,
                               const char* valueKey, char* out, size_t outLen) {
    if (outLen == 0) return;
    out[0] = 0;
    if (!body || !anchor) return;
    const char* p = std::strstr(body, anchor);
    if (!p) return;
    p += std::strlen(anchor);
    if (valueKey) {
        p = std::strstr(p, valueKey);
        if (!p) return;
        p += std::strlen(valueKey);
    }
    const char* colon = std::strchr(p, ':');
    if (!colon) return;
    const char* q = std::strchr(colon, '"');
    if (!q) return;
    q++;
    size_t i = 0;
    for (; q[i] && q[i] != '"' && i + 1 < outLen; i++) out[i] = q[i];
    out[i] = 0;
}

// Pull a display name from the probe body for a given type. projectMM exposes
// deviceName as a CONTROL OBJECT in /api/state —
// {"name":"deviceName","type":"text","value":"Bench P4"} — so anchor on
// "deviceName" then read the next "value" (NOT the first quoted token, which is the
// "text" type field). WLED's /json/info has a top-level bare "name" string. Leaves
// `out` empty when no name is found (caller falls back to the IP).
inline void extractDeviceName(DevType type, const char* body,
                              char* out, size_t outLen) {
    if (outLen == 0) return;
    out[0] = 0;
    if (!body) return;
    if (type == DevType::ProjectMM) {
        extractStringAfter(body, "\"deviceName\"", "\"value\"", out, outLen);
    } else if (type == DevType::Wled) {
        extractStringAfter(body, "\"name\"", nullptr, out, outLen);
    }
}

}  // namespace mm
