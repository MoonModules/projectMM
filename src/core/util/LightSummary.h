#pragma once

#include <cstdint>

namespace mm {

/// A small plain-data summary of the light pipeline's output, readable from core without any light-domain type.
///
/// @moreinfo
///
/// ## Who fills it and who reads it
///
/// The light domain's `Drivers` container produces it, and domain-neutral consumers read it: the WLED `/json` shim and MQTT, which report the real device shape.
/// This is the shared-struct pull pattern the architecture page describes under data exchange, the same shape as `AudioFrame`.
/// The producer owns one plain struct overwritten in place on each rebuild, and a consumer holds a `const LightSummary*` and reads it, with no allocation and no event bus.
///
/// It stays domain-neutral on purpose: plain `uint32_t` and `uint8_t` with no light typedefs, so it lives in core beside its consumers.
/// `lightCount` is a `uint32_t` to hold any count on any board, the light-side `nrOfLightsType` being a `uint16_t` without PSRAM and a `uint32_t` with it.
///
/// ## Extendable by design
///
/// Adding a field, say estimated power or a segment count, means the producer fills it in one place and every consumer that wants it reads it, with no new seam.
/// Keep it a flat struct of small integers.
struct LightSummary {
    uint32_t lightCount = 0;              ///< total physical lights driven (Layer::physicalLightCount()).
    uint8_t  channelsPerLight = 3;        ///< 3 = RGB, 4 = RGBW, more = multi-channel DMX fixtures.
};

} // namespace mm
