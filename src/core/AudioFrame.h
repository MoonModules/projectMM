#pragma once

#include <cstdint>

namespace mm {

// One snapshot of analysed audio, produced by AudioModule (src/core/AudioModule.h)
// once per render tick and consumed by audio-reactive effects (AudioVolumeEffect,
// AudioSpectrumEffect). The producer/consumer-via-plain-struct model the codebase
// already uses (PreviewDriver writes pixels HttpServer reads); the struct is the
// whole contract between the two, so effects never touch I2S or the FFT.
//
// POD by design: a flat value type the producer fills and the consumer reads, no
// ownership, no methods — copy it or hold a `const AudioFrame*` to the module's
// latest. All fields are pre-scaled to small integers so an effect does integer
// math straight off them (the hot-path rule); the float FFT magnitudes never
// leave the module.
struct AudioFrame {
    uint16_t level = 0;        // overall sound level (RMS), 0..255-ish — the VU value
    uint16_t peakHz = 0;       // dominant frequency this frame, in Hz (0 = none)
    uint16_t peakMag = 0;      // magnitude of that peak (gates the peakHz update)
    uint8_t  bands[16] = {};   // 16 log-spaced frequency-band magnitudes, 0..255
                               // (bass = bands[0], treble = bands[15])
};

} // namespace mm
