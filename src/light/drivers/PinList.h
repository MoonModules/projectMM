#pragma once

#include "light/light_types.h"  // nrOfLightsType

#include <cstdint>
#include <cstdlib>  // std::strtol

namespace mm {

// Shared pin/count list parsing for multi-output LED drivers — RmtLedDriver
// (one RMT channel per pin) and LcdLedDriver (one LCD_CAM lane per pin) both
// drive consecutive slices of the source buffer from the same two text
// controls, so the parsers live once here (extracted when the second user
// landed, per the increment-1 plan's "concrete first" rule). Both return
// nullptr on success or a static error literal the caller feeds straight into
// setStatus(); both are strtol-based like parseDottedQuad (Control.h) and
// fully host-testable (unit_RmtLedDriver_pins.cpp pins them for both drivers).

// Parse "18,17,16" into out[0..maxPins). Spaces around tokens are fine
// (strtol skips them). Rejects empty input, bad tokens, trailing commas,
// duplicates, and more than maxPins entries (the chip's channel/lane cap).
inline const char* parsePinList(const char* s, uint16_t* out, uint8_t maxPins,
                                uint8_t& nOut) {
    nOut = 0;
    if (!s || !*s) return "invalid pin list";
    const char* p = s;
    while (true) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p || v < 0 || v > 0xFFFF) return "invalid pin list";
        while (*end == ' ') end++;
        if (nOut >= maxPins) return "too many pins for this chip";
        for (uint8_t i = 0; i < nOut; i++)
            if (out[i] == static_cast<uint16_t>(v)) return "duplicate pin";
        out[nOut++] = static_cast<uint16_t>(v);
        if (*end == '\0') return nullptr;
        if (*end != ',') return "invalid pin list";
        p = end + 1;
    }
}

// Fill counts[0..nPins) from "100,100,50" (may be empty or shorter than the
// pin list; extra entries beyond nPins are ignored — a stale longer list
// after pins shrank is not an error). Explicit counts are clamped so the
// running sum never exceeds totalLights; the unassigned remainder splits
// evenly over the unlisted pins, last pin takes the rounding remainder.
inline const char* assignCounts(const char* s, uint8_t nPins,
                                nrOfLightsType totalLights, nrOfLightsType* counts) {
    for (uint8_t i = 0; i < nPins; i++) counts[i] = 0;
    nrOfLightsType remaining = totalLights;
    uint8_t nExplicit = 0;
    const char* p = s;
    while (p && *p && nExplicit < nPins) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p || v < 0) return "invalid ledsPerPin list";
        while (*end == ' ') end++;
        const nrOfLightsType c =
            (v > static_cast<long>(remaining)) ? remaining
                                               : static_cast<nrOfLightsType>(v);
        counts[nExplicit++] = c;
        remaining = static_cast<nrOfLightsType>(remaining - c);
        if (*end == '\0') break;
        if (*end != ',') return "invalid ledsPerPin list";
        p = end + 1;
    }
    const uint8_t nRemaining = static_cast<uint8_t>(nPins - nExplicit);
    if (nRemaining > 0) {
        const nrOfLightsType per = static_cast<nrOfLightsType>(remaining / nRemaining);
        for (uint8_t i = nExplicit; i < nPins; i++) counts[i] = per;
        counts[nPins - 1] = static_cast<nrOfLightsType>(
            counts[nPins - 1] + (remaining - per * nRemaining));
    }
    return nullptr;
}

} // namespace mm
