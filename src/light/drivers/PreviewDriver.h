#pragma once

#include "light/drivers/Drivers.h"
#include "core/PreviewFrame.h"
#include "platform/platform.h"

namespace mm {

class PreviewDriver : public DriverBase {
public:
    // 24 fps keeps the preview WebSocket broadcast well within the render-tick
    // budget while staying visually smooth (the browser caches geometry between
    // frames). User-tunable 1-60 via the "fps" control.
    uint8_t fps = 24;

    // Preview detail: 1 = coarse, 2 = medium, 3 = fine. Higher = more voxels in
    // the downsampled frame = a denser point cloud, at the cost of a larger (but
    // still send-buffer-safe) WebSocket payload. Lets both coarse and fine
    // previews be tested live without reflashing.
    uint8_t detail = 3;

    // UI render hint: when true the browser reconstructs (block-replicates) the
    // downsampled frame back to the original physical grid resolution, so the
    // preview shows the same voxel count as the real layout. Purely client-side
    // — the wire payload is the downsampled frame either way. The frame always
    // carries the original dimensions; this flag just tells the UI to use them.
    bool decompress = true;

    void setPreviewFrame(PreviewFrame* f) { frame_ = f; }

    void onBuildControls() override {
        controls_.addUint8("fps", fps, 1, 60);
        controls_.addUint8("detail", detail, 1, 3);
        controls_.addBool("decompress", decompress);
    }

    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
    }

    void onAllocateMemory() override {
        // Owned downsample buffer, sized once to the largest possible budget
        // (detail = 3). The preview frame is sent over WebSocket in a single
        // non-blocking write, so even the finest setting must fit lwIP's
        // 11520 B TCP send buffer — MAX_PREVIEW_VOXELS keeps the payload safe.
        downsampled_.allocate(MAX_PREVIEW_VOXELS, 3);
        setDynamicBytes(downsampled_.bytes());
        MoonModule::onAllocateMemory();
    }

    void loop() override {
        if (fps == 0) return;
        uint32_t now = platform::millis();
        uint32_t interval = 1000 / fps;
        if (now - lastSendTime_ < interval) return;  // rate-limit gate
        lastSendTime_ = now;
        renderFrame();
    }

    // Produce one downsampled preview frame, bypassing the loop()'s fps
    // rate-limit. Public so tests can drive frame production deterministically
    // without sleeping for the rate-limit interval.
    void renderFrame() {
        if (!sourceBuffer_ || !sourceBuffer_->data() || !frame_ || !layer_) return;
        if (!downsampled_.data()) return;

        // Dimensions read from Layer each frame so the preview tracks runtime resizes.
        lengthType w = layer_->physicalWidth();
        lengthType h = layer_->physicalHeight();
        lengthType d = layer_->physicalDepth();
        if (w <= 0 || h <= 0 || d <= 0) return;

        size_t budget = voxelBudget();

        // Pick the smallest stride whose downsampled voxel count fits the budget.
        uint8_t stride = 1;
        lengthType dw = w, dh = h, dd = d;
        while (static_cast<size_t>(dw) * dh * dd > budget) {
            stride++;
            dw = (w + stride - 1) / stride;
            dh = (h + stride - 1) / stride;
            dd = (d + stride - 1) / stride;
        }

        // Downsample: for each stride-sized cell, take the brightest voxel
        // (max per channel across the cell). A simple strided pick misses all
        // lit pixels whenever the effect pattern falls between stride steps,
        // producing empty frames. Max-pooling guarantees a cell is non-black
        // whenever any voxel inside it is lit.
        const uint8_t* src = sourceBuffer_->data();
        uint8_t cpl = sourceBuffer_->channelsPerLight();
        size_t lightCount = sourceBuffer_->count();
        uint8_t* dst = downsampled_.data();
        size_t di = 0;
        for (lengthType cz = 0; cz < dd; cz++) {
            for (lengthType cy = 0; cy < dh; cy++) {
                for (lengthType cx = 0; cx < dw; cx++) {
                    // Pool over the stride×stride×stride cell
                    uint8_t maxR = 0, maxG = 0, maxB = 0;
                    for (uint8_t sz = 0; sz < stride; sz++) {
                        lengthType oz = cz * stride + sz;
                        if (oz >= d) continue;
                        for (uint8_t sy = 0; sy < stride; sy++) {
                            lengthType oy = cy * stride + sy;
                            if (oy >= h) continue;
                            for (uint8_t sx = 0; sx < stride; sx++) {
                                lengthType ox = cx * stride + sx;
                                if (ox >= w) continue;
                                size_t lightIdx = static_cast<size_t>(oz) * h * w
                                                + static_cast<size_t>(oy) * w + ox;
                                if (lightIdx >= lightCount) continue;
                                size_t srcIdx = lightIdx * cpl;
                                if (src[srcIdx]     > maxR) maxR = src[srcIdx];
                                if (cpl >= 2 && src[srcIdx+1] > maxG) maxG = src[srcIdx+1];
                                if (cpl >= 3 && src[srcIdx+2] > maxB) maxB = src[srcIdx+2];
                            }
                        }
                    }
                    dst[di * 3 + 0] = maxR;
                    dst[di * 3 + 1] = maxG;
                    dst[di * 3 + 2] = maxB;
                    di++;
                }
            }
        }

        frame_->data = dst;
        frame_->dataLen = di * 3;
        frame_->width = dw;
        frame_->height = dh;
        frame_->depth = dd;
        frame_->origWidth = w;
        frame_->origHeight = h;
        frame_->origDepth = d;
        frame_->fps = fps;
        frame_->ready = true;
    }

private:
    // Largest voxel budget (detail = 3): 1849 × 3 B = 5547 B payload + ~11 B
    // headers ≈ 5558 B — fits the 11520 B lwIP TCP send buffer with comfortable
    // headroom (see CONFIG_LWIP_TCP_SND_BUF_DEFAULT in sdkconfig.defaults), so the
    // non-blocking preview write completes whole rather than partial-sending.
    static constexpr size_t MAX_PREVIEW_VOXELS = 1849;

    // Voxel budget per detail level. The values are chosen so that on a 128-axis
    // grid the adaptive stride lands on a distinct value per level:
    //   1 → stride 8 (16×16),  2 → stride 4 (32×32),  3 → stride 3 (43×43).
    size_t voxelBudget() const {
        switch (detail) {
            case 1:  return 256;
            case 3:  return MAX_PREVIEW_VOXELS;  // 1849
            default: return 1024;   // detail == 2
        }
    }

    Buffer* sourceBuffer_ = nullptr;
    PreviewFrame* frame_ = nullptr;
    Buffer downsampled_;          // owned; strided RGB copy of the source buffer
    uint32_t lastSendTime_ = 0;
};

} // namespace mm
