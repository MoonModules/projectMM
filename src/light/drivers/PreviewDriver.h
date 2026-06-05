#pragma once

#include "light/drivers/Drivers.h"
#include "light/light_types.h"  // lengthType
#include "core/BinaryBroadcaster.h"
#include "platform/platform.h"

namespace mm {

// Zero-copy preview frame: the latest downsampled RGB output plus the
// dimensions the UI needs. PreviewDriver fills it each frame and pushes it to
// the WS broadcaster (it owns the wire format); no other module reads it.
// Lives here, with its only producer/consumer — single-threaded scheduler, no
// lock needed.
struct PreviewFrame {
    const uint8_t* data = nullptr;  // points to PreviewDriver's downsample buffer (no ownership)
    size_t dataLen = 0;
    // Dimensions of the (downsampled) data actually in `data`.
    lengthType width = 0;
    lengthType height = 0;
    lengthType depth = 0;
    // Dimensions of the original physical grid before downsampling. Equal to
    // width/height/depth when no downsampling occurred. Sent so the UI can
    // optionally reconstruct (block-replicate) the preview at full resolution.
    lengthType origWidth = 0;
    lengthType origHeight = 0;
    lengthType origDepth = 0;
    uint8_t fps = 20;
};

class PreviewDriver : public DriverBase {
public:
    // The 3D preview the web UI renders streams from this driver. Deleting or
    // replacing it from the UI would silently kill that preview, so it opts
    // out of user-editing — it stays a fixed child of Drivers. (A user who
    // genuinely wants it gone can still remove it via persistence / MoonDeck;
    // this only hides the UI delete/replace affordance.)
    bool userEditable() const override { return false; }

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

    // The sink each produced frame is pushed to (HttpServerModule, as a
    // BinaryBroadcaster). Wired in main.cpp. Light depends only on the
    // BinaryBroadcaster interface, not the concrete HTTP server.
    void setBroadcaster(BinaryBroadcaster* b) { broadcaster_ = b; }

    void onBuildControls() override {
        controls_.addUint8("fps", fps, 1, 60);
        controls_.addUint8("detail", detail, 1, 3);
        controls_.addBool("decompress", decompress);
    }

    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
    }

    void onBuildState() override {
        // Owned downsample buffer, sized once to the largest possible budget
        // (detail = 3). The preview frame is sent over WebSocket in a single
        // non-blocking write, so even the finest setting must fit lwIP's
        // 11520 B TCP send buffer — MAX_PREVIEW_VOXELS keeps the payload safe.
        downsampled_.allocate(MAX_PREVIEW_VOXELS, 3);
        setDynamicBytes(downsampled_.bytes());
        MoonModule::onBuildState();
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

        pushFrame();
    }

    // Build the 13-byte preview header and push header+payload to the WS
    // broadcaster. The preview wire format lives here (light domain), not in
    // the HTTP server — HttpServer only knows "broadcast these bytes".
    //   [0x02][dw16][dh16][dd16][ow16][oh16][od16] then the RGB payload.
    // All dimensions uint16 little-endian. The payload chunk points at our own
    // downsample buffer (no copy); broadcastBinary writes it before returning.
    void pushFrame() {
        if (!broadcaster_ || !frame_->data || frame_->dataLen == 0) return;
        uint8_t header[13];
        header[0] = 0x02;
        auto put16 = [&](int at, lengthType v) {
            header[at]     = static_cast<uint8_t>(v & 0xFF);
            header[at + 1] = static_cast<uint8_t>(v >> 8);
        };
        put16(1, frame_->width);
        put16(3, frame_->height);
        put16(5, frame_->depth);
        put16(7, frame_->origWidth);
        put16(9, frame_->origHeight);
        put16(11, frame_->origDepth);

        const platform::WriteChunk payload[] = {
            { header,       sizeof(header) },
            { frame_->data, frame_->dataLen },
        };
        broadcaster_->broadcastBinary(payload, 2);
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
    BinaryBroadcaster* broadcaster_ = nullptr;
    Buffer downsampled_;          // owned; max-pooled RGB output buffer (one entry per stride cell)
    uint32_t lastSendTime_ = 0;
};

} // namespace mm
