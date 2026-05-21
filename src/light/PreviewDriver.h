#pragma once

#include "light/DriverGroup.h"
#include "core/PreviewFrame.h"
#include "platform/platform.h"

namespace mm {

class PreviewDriver : public DriverBase {
public:
    uint8_t fps = 20;

    void setPreviewFrame(PreviewFrame* f) { frame_ = f; }

    void onBuildControls() override {
        controls_.addUint8("fps", fps, 1, 60);
    }

    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
    }

    void loop() override {
        if (!sourceBuffer_ || !sourceBuffer_->data() || !frame_ || !layer_) return;
        if (fps == 0) return;

        uint32_t now = platform::millis();
        uint32_t interval = 1000 / fps;
        if (now - lastSendTime_ < interval) return;
        lastSendTime_ = now;

        // Read dimensions from Layer each frame so the preview tracks runtime grid resizes.
        // Zero-copy: point directly at the source buffer.
        frame_->data = sourceBuffer_->data();
        frame_->dataLen = sourceBuffer_->bytes();
        frame_->width = layer_->physicalWidth();
        frame_->height = layer_->physicalHeight();
        frame_->depth = layer_->physicalDepth();
        frame_->fps = fps;
        frame_->ready = true;
    }

private:
    Buffer* sourceBuffer_ = nullptr;
    PreviewFrame* frame_ = nullptr;
    uint32_t lastSendTime_ = 0;
};

} // namespace mm
