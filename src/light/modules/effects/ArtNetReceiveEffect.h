#pragma once

#include "light/EffectBase.h"

namespace mm::light {

// ArtNet receive effect — receives ArtNet UDP and writes pixel data
// into the layer buffer like any other effect.
// Currently a stub: fills buffer with a test pattern until platform
// networking is implemented.
class ArtNetReceiveEffect : public EffectBase {
public:
    const char* name() const override { return "ArtNet Receive"; }

    void addControls() override {
        universeIdx_ = MoonModule::addControl("universe", uint16_t(0), uint16_t(0), uint16_t(32767));
        portIdx_     = MoonModule::addControl("port",     uint16_t(6454), uint16_t(1), uint16_t(65535));
    }

    void loop() override {
        // TODO: when platform networking is available, poll for ArtNet
        // packets here (synchronously, non-blocking) and copy pixel
        // data into the buffer.

        // Stub: fill with a dim pattern based on universe to prove
        // the effect structure works.
        auto px = pixels();
        uint8_t uni = static_cast<uint8_t>(control(universeIdx_)->u16.value & 0xFF);
        for (size_t i = 0; i < px.size(); ++i) {
            px[i] = RGB{uni, static_cast<uint8_t>(i & 0xFF), 0};
        }
    }

private:
    uint8_t universeIdx_ = 0;
    uint8_t portIdx_ = 0;
};

} // namespace mm::light
