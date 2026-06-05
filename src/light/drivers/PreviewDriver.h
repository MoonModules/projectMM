#pragma once

#include "light/drivers/Drivers.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType
#include "core/BinaryBroadcaster.h"
#include "platform/platform.h"

namespace mm {

// Streams a true-shape 3D preview to the web UI over the binary WebSocket.
//
// The preview is a POINT LIST, not a dense grid: only the real lights are sent,
// at their real (x,y,z) positions. This is the proven MoonLight model (virtual
// grid → physical sparse lights; positions sent once at mapping time, channels
// per frame). Two message types — PreviewDriver owns both wire formats; the
// HTTP server is a domain-neutral BinaryBroadcaster that just writes the bytes:
//
//   0x03 coordinate table (one-time, on every LUT rebuild + periodic re-send so
//        new clients catch it):
//        [0x03][count:u16][bx:u8][by:u8][bz:u8][stride:u16][(x,y,z):u8×3 × count]
//        bx/by/bz = bounding-box extent (for client centring); positions are
//        1 byte/axis (a layout box ≤255/axis is the realistic case).
//
//   0x02 per-frame channels: [0x02][count:u16][stride:u16][(r,g,b) × count]
//        RGB by driver index, every `stride`-th light. The browser positions
//        triple i at coord-table entry i*stride.
//
// `count` here is the number of points actually sent = ceil(lightCount/stride).
// stride>1 only when lightCount*3 would exceed the send-buffer cap (a large
// dense grid); sparse layouts (sphere) send every light exactly (stride 1).
class PreviewDriver : public DriverBase {
public:
    // The 3D preview the web UI renders streams from this driver. Deleting or
    // replacing it from the UI would silently kill that preview, so it opts out
    // of user-editing — it stays a fixed child of Drivers.
    bool userEditable() const override { return false; }

    // Preview stream rate (Hz), independent of render FPS. User-tunable 1-60.
    uint8_t fps = 24;

    // The sink each message is pushed to (HttpServerModule, as a
    // BinaryBroadcaster). Wired in main.cpp. Light depends only on the
    // interface, not the concrete HTTP server.
    void setBroadcaster(BinaryBroadcaster* b) { broadcaster_ = b; }

    void onBuildControls() override {
        controls_.addUint8("fps", fps, 1, 60);
    }

    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
    }

    // A rebuild (layout add/replace/remove, resize, modifier change) ran — the
    // light set / positions may have changed, so rebuild + broadcast the
    // coordinate table. This is the MoonLight "positions once at mapping time".
    void onBuildState() override {
        buildAndSendCoordTable();
        MoonModule::onBuildState();
    }

    void loop() override {
        if (fps == 0) return;
        uint32_t now = platform::millis();
        uint32_t interval = 1000 / fps;
        if (now - lastSendTime_ < interval) return;  // rate-limit gate
        lastSendTime_ = now;

        // Rebuild + re-broadcast the coordinate table ~once per second (and
        // immediately while it's still empty). The ~1 Hz cadence lets a client
        // that connected after the last onBuildState catch the positions, and
        // rebuilding (not just re-sending a cache) self-heals the case where the
        // layout/source wasn't wired yet at onBuildState time — cheap on the
        // cold path and idempotent on the client.
        if (coordCount_ == 0 || now - lastCoordTime_ >= 1000) {
            buildAndSendCoordTable();
        }

        sendFrame();
    }

    // Build (or rebuild) the cached coordinate table from the layout's real
    // lights and broadcast it. Public so tests can drive it deterministically.
    void buildAndSendCoordTable() {
        coordCount_ = 0;
        if (!layer_ || !layer_->layouts()) return;
        Layouts* layouts = layer_->layouts();
        nrOfLightsType n = layouts->totalLightCount();
        if (n == 0) return;
        stride_ = computeStride(n);
        coordCount_ = (n + stride_ - 1) / stride_;  // points actually sent

        // Positions are 1 byte/axis. To support layouts whose bounding box
        // exceeds 255 on an axis (a 512-wide grid, say), scale every axis by the
        // same factor so the largest box edge maps to 255 — preserving aspect
        // ratio. For boxes ≤255/axis the factor is 1 (exact integer positions).
        // The header carries the SCALED box extents, so the browser's centring
        // (divide by max axis) stays consistent with the packed coordinates.
        lengthType maxEdge = layer_->physicalWidth();
        if (layer_->physicalHeight() > maxEdge) maxEdge = layer_->physicalHeight();
        if (layer_->physicalDepth() > maxEdge) maxEdge = layer_->physicalDepth();
        if (maxEdge < 1) maxEdge = 1;
        posScale_ = (maxEdge > 255) ? maxEdge : 0;   // 0 = no scaling (1:1)
        bx_ = scaleAxis(layer_->physicalWidth());
        by_ = scaleAxis(layer_->physicalHeight());
        bz_ = scaleAxis(layer_->physicalDepth());

        // Header: [0x03][count:u16][bx][by][bz][stride:u16]
        uint8_t* h = coordHeader_;
        h[0] = 0x03;
        h[1] = static_cast<uint8_t>(coordCount_ & 0xFF);
        h[2] = static_cast<uint8_t>(coordCount_ >> 8);
        h[3] = bx_; h[4] = by_; h[5] = bz_;
        h[6] = static_cast<uint8_t>(stride_ & 0xFF);
        h[7] = static_cast<uint8_t>(stride_ >> 8);

        // Pack (x,y,z) for every stride-th light, in forEachCoord (driver) order.
        if (!coords_.data() || coords_.count() < coordCount_) {
            coords_.allocate(MAX_PREVIEW_POINTS, 3);  // owned u8×3 position buffer
        }
        if (!coords_.data()) { coordCount_ = 0; return; }
        struct PackCtx { PreviewDriver* self; uint8_t* dst; nrOfLightsType stride; nrOfLightsType out; nrOfLightsType cap; };
        PackCtx pc{this, coords_.data(), stride_, 0, coordCount_};
        layouts->forEachCoord([](void* c, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
            auto* p = static_cast<PackCtx*>(c);
            if (idx % p->stride != 0) return;
            if (p->out >= p->cap) return;
            uint8_t* d = p->dst + static_cast<size_t>(p->out) * 3;
            d[0] = p->self->scaleAxis(x); d[1] = p->self->scaleAxis(y); d[2] = p->self->scaleAxis(z);
            p->out++;
        }, &pc);

        lastCoordTime_ = platform::millis();
        sendCoordTable();
    }

    // Produce + push one per-frame 0x02 RGB message. Public so tests can drive
    // it without the loop() rate-limit.
    void sendFrame() {
        if (!broadcaster_ || !sourceBuffer_ || !sourceBuffer_->data() || coordCount_ == 0) return;
        const uint8_t* src = sourceBuffer_->data();
        uint8_t cpl = sourceBuffer_->channelsPerLight();
        nrOfLightsType n = sourceBuffer_->count();

        // RGB scratch sized to the sent-point count; pick every stride-th light.
        if (!rgb_.data() || rgb_.count() < coordCount_) rgb_.allocate(MAX_PREVIEW_POINTS, 3);
        if (!rgb_.data()) return;
        uint8_t* dst = rgb_.data();
        nrOfLightsType out = 0;
        for (nrOfLightsType i = 0; i < n && out < coordCount_; i += stride_) {
            const uint8_t* s = src + static_cast<size_t>(i) * cpl;
            dst[out * 3 + 0] = s[0];
            dst[out * 3 + 1] = cpl >= 2 ? s[1] : 0;
            dst[out * 3 + 2] = cpl >= 3 ? s[2] : 0;
            out++;
        }

        uint8_t header[5];
        header[0] = 0x02;
        header[1] = static_cast<uint8_t>(out & 0xFF);
        header[2] = static_cast<uint8_t>(out >> 8);
        header[3] = static_cast<uint8_t>(stride_ & 0xFF);
        header[4] = static_cast<uint8_t>(stride_ >> 8);

        const platform::WriteChunk payload[] = {
            { header, sizeof(header) },
            { dst, static_cast<size_t>(out) * 3 },
        };
        broadcaster_->broadcastBinary(payload, 2);
    }

private:
    // Send-buffer cap: a preview message is one non-blocking writev. lwIP's TCP
    // send buffer is 11520 B (CONFIG_LWIP_TCP_SND_BUF_DEFAULT), but it is NOT
    // reliably all-free at send time (the render task shares it, frames go out at
    // the render rate), so a payload near the ceiling partial-writes — and a
    // partial write drops the WS connection (broadcastBinary closes on Partial),
    // making the preview flicker/blank. Cap at 1800 points → 1800×3 = 5400 B
    // payload + headers ≈ 5.4 KB, well under half the buffer — the same safe
    // headroom the pre-point-list code used (1849 voxels). Larger layouts stride
    // down to fit.
    static constexpr nrOfLightsType MAX_PREVIEW_POINTS = 1800;

    // Smallest stride whose sent-point count fits the cap. stride 1 (exact) for
    // anything ≤ MAX_PREVIEW_POINTS — every sparse layout and small grid.
    static nrOfLightsType computeStride(nrOfLightsType n) {
        nrOfLightsType s = 1;
        while ((n + s - 1) / s > MAX_PREVIEW_POINTS) s++;
        return s;
    }

    // Map an axis coordinate into the 0..255 byte range. posScale_ == 0 means
    // the box already fits (1:1, exact integer positions); otherwise scale by
    // 255/posScale_ (posScale_ = the largest box edge), preserving aspect ratio
    // so a >255 axis isn't silently flattened onto the 255 plane.
    uint8_t scaleAxis(lengthType v) const {
        if (v < 0) return 0;
        int32_t s = posScale_ ? (static_cast<int32_t>(v) * 255 / posScale_) : v;
        return s > 255 ? 255 : static_cast<uint8_t>(s);
    }

    void sendCoordTable() {
        if (!broadcaster_ || coordCount_ == 0 || !coords_.data()) return;
        const platform::WriteChunk payload[] = {
            { coordHeader_, sizeof(coordHeader_) },
            { coords_.data(), static_cast<size_t>(coordCount_) * 3 },
        };
        broadcaster_->broadcastBinary(payload, 2);
    }

    Buffer* sourceBuffer_ = nullptr;
    BinaryBroadcaster* broadcaster_ = nullptr;
    Buffer coords_;               // owned; u8×3 positions, one per sent point
    Buffer rgb_;                  // owned; u8×3 colours, one per sent point
    uint8_t coordHeader_[8] = {};
    nrOfLightsType coordCount_ = 0;   // points actually sent (after stride)
    nrOfLightsType stride_ = 1;
    uint8_t bx_ = 0, by_ = 0, bz_ = 0;
    int32_t posScale_ = 0;            // 0 = positions 1:1; else largest box edge (>255) to scale by
    uint32_t lastSendTime_ = 0;
    uint32_t lastCoordTime_ = 0;
};

} // namespace mm
