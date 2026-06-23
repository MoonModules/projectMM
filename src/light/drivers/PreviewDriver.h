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
//   0x03 coordinate table (sent when the geometry changes — every LUT/layout rebuild
//        via onBuildState — and when a new client connects, so a refresh gets it; never
//        per-frame):
//        [0x03][count:u32][bx:u8][by:u8][bz:u8][stride:u16][(x,y,z):u8×3 × count]
//        bx/by/bz = bounding-box extent (for client centring); positions are
//        1 byte/axis (a layout box ≤255/axis is the realistic case). count is u32 so a
//        >65535-light panel (big ArtNet/HUB75 walls) isn't capped by the wire format —
//        it matches nrOfLightsType (u32 on PSRAM boards).
//
//   0x02 per-frame channels: [0x02][count:u32][stride:u16][(r,g,b) × count]
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

        // The coordinate table is (re)streamed only when the geometry changes (onBuildState — a
        // resize / LUT rebuild), when a new client connects (clientGeneration bump, so a page
        // refresh gets positions immediately), when the adaptive factor changes, or while a
        // previous stream didn't reach every client (coordPending_ retry). NOT per frame: the
        // colour frames below reference the last-streamed positions. coordCount_==0 = cold start.
        uint32_t gen = broadcaster_ ? broadcaster_->clientGeneration() : 0;
        if (coordCount_ == 0 || gen != lastClientGen_ || coordPending_) {
            lastClientGen_ = gen;
            buildAndSendCoordTable();   // streams positions; sets coordPending_ if not all clients got it
        }

        // Hold colour frames until the browser has the matching coordinate table — a 0x02 whose
        // count/stride don't match the last 0x03 is skipped by the browser, and streaming it
        // anyway wastes the link. sendFrame() returns false if a client couldn't take the whole
        // frame (it gets closed); treat that as "link can't keep up" for the adaptive step.
        bool frameOk = true;
        if (!coordPending_) frameOk = sendFrame();

        // Adaptive resolution. The streamed send is all-or-nothing per client (a client that
        // can't take the whole frame is closed), so a NOT-all-sent result — for the colour frame
        // (frameOk false) OR the coord table (coordPending_) — means the link can't sustain this
        // resolution: coarsen (downscale_++) after a short run so the rebuilt lattice sends fewer
        // points. A sustained all-sent run refines back toward full resolution (downscale_--).
        // Hysteresis via the streaks stops oscillation; the factor rides the wire stride field to
        // the browser's status line. (On a memory-tight board the coord table is simply too big
        // to push until downscaled — coordPending_ is that "too big" signal.)
        const bool linkStruggling = coordPending_ || !frameOk;
        if (linkStruggling) {
            cleanStreak_ = 0;
            if (++slowStreak_ >= kDownscaleAfterSlow && downscale_ < 64) {
                slowStreak_ = 0;
                downscale_++;
                buildAndSendCoordTable();
            }
        } else {
            slowStreak_ = 0;
            if (downscale_ > 1 && ++cleanStreak_ >= kUpscaleAfterFast) {
                cleanStreak_ = 0;
                downscale_--;
                buildAndSendCoordTable();
            }
        }
    }

    // Build (or rebuild) the cached coordinate table from the layout's real
    // lights and broadcast it. Public so tests can drive it deterministically.
    void buildAndSendCoordTable() {
        coordCount_ = 0;
        if (!layer_ || !layer_->layouts()) return;
        Layouts* layouts = layer_->layouts();
        nrOfLightsType n = layouts->totalLightCount();
        if (n == 0) return;

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

        // Per-axis downsample step s (lattice skip x%s && y%s && z%s). The cell count of the
        // bounding box is the upper bound on kept lights, so grow s until it fits the cap — but
        // ONLY when the layout has more lights than the cap (a sparse layout — big box, few
        // lights — fits at s==1 and must not be downsampled for its box size alone). The wire
        // "stride" field carries s to the browser (1 = full res; >1 = "1/s shown, link limited").
        const lengthType ax = layer_->physicalWidth()  > 0 ? layer_->physicalWidth()  : 1;
        const lengthType ay = layer_->physicalHeight() > 0 ? layer_->physicalHeight() : 1;
        const lengthType az = layer_->physicalDepth()  > 0 ? layer_->physicalDepth()  : 1;
        nrOfLightsType s = 1;
        if (n > MAX_PREVIEW_POINTS) {
            auto latticeCount = [&](nrOfLightsType step) {
                nrOfLightsType cx = (ax + step - 1) / step, cy = (ay + step - 1) / step,
                               cz = (az + step - 1) / step;
                return static_cast<uint32_t>(cx) * cy * cz;
            };
            while (latticeCount(s) > MAX_PREVIEW_POINTS) s++;
        }
        if (s < downscale_) s = downscale_;   // adaptive: never finer than the link sustains
        previewStride_ = s;

        // Count the lights the lattice keeps (one cheap forEachCoord pass — no allocation). This
        // is the 0x03 count, and the per-frame colour pass re-applies the SAME predicate over the
        // SAME forEachCoord order, so colour[k] lines up with coord[k] with no stored index map.
        struct CountCtx { nrOfLightsType s, out; };
        CountCtx cc{s, 0};
        layouts->forEachCoord([](void* c, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
            auto* p = static_cast<CountCtx*>(c);
            if (x % p->s == 0 && y % p->s == 0 && z % p->s == 0) p->out++;
        }, &cc);
        coordCount_ = cc.out;
        if (coordCount_ == 0) { coordPending_ = false; return; }

        // STREAM the coordinate table: WS header (count + box + stride), then push each kept
        // light's scaled (x,y,z) straight from forEachCoord — no coords_ buffer ever exists.
        // (positions are sent rarely: on geometry change / new client / downscale change.)
        uint8_t h[10];
        h[0] = 0x03;
        h[1] = static_cast<uint8_t>(coordCount_ & 0xFF);
        h[2] = static_cast<uint8_t>((coordCount_ >> 8) & 0xFF);
        h[3] = static_cast<uint8_t>((coordCount_ >> 16) & 0xFF);
        h[4] = static_cast<uint8_t>((coordCount_ >> 24) & 0xFF);
        h[5] = bx_; h[6] = by_; h[7] = bz_;
        h[8] = static_cast<uint8_t>(s & 0xFF);
        h[9] = static_cast<uint8_t>(s >> 8);

        if (!broadcaster_) { coordPending_ = true; return; }
        broadcaster_->beginBinaryFrame(sizeof(h) + static_cast<size_t>(coordCount_) * 3);
        broadcaster_->pushBinaryFrame(h, sizeof(h));
        // Push positions in small slices: forEachCoord fills a stack scratch, flushed when full.
        struct StreamCtx {
            PreviewDriver* self; mm::BinaryBroadcaster* bc; nrOfLightsType s;
            uint8_t buf[1536]; uint16_t fill;
        };
        StreamCtx sc{this, broadcaster_, s, {}, 0};
        layouts->forEachCoord([](void* c, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
            auto* p = static_cast<StreamCtx*>(c);
            if (x % p->s != 0 || y % p->s != 0 || z % p->s != 0) return;
            p->buf[p->fill++] = p->self->scaleAxis(x);
            p->buf[p->fill++] = p->self->scaleAxis(y);
            p->buf[p->fill++] = p->self->scaleAxis(z);
            if (p->fill > sizeof(p->buf) - 3) { p->bc->pushBinaryFrame(p->buf, p->fill); p->fill = 0; }
        }, &sc);
        if (sc.fill) broadcaster_->pushBinaryFrame(sc.buf, sc.fill);
        // The coord table must reach the browser before colour frames carrying the new count
        // (else the browser's count-mismatch guard skips them). endBinaryFrame() reports whether
        // every client got it; loop() retries while pending and withholds colour frames.
        coordPending_ = !broadcaster_->endBinaryFrame();
    }

    // STREAM one per-frame 0x02 RGB message from the producer buffer — no intermediate buffer.
    // Returns whether every client got it (false → loop() drives adaptive downscaling). Public
    // so tests can drive it without the loop() rate-limit.
    bool sendFrame() {
        if (!broadcaster_ || !sourceBuffer_ || !sourceBuffer_->data() || coordCount_ == 0) return false;
        const uint8_t* src = sourceBuffer_->data();
        const uint8_t cpl = sourceBuffer_->channelsPerLight();
        const nrOfLightsType n = sourceBuffer_->count();
        const nrOfLightsType s = previewStride_;

        // Header: [0x02][count:u32 LE][stride:u16 LE]  (7 bytes). count = the kept lights.
        uint8_t header[7];
        header[0] = 0x02;
        header[1] = static_cast<uint8_t>(coordCount_ & 0xFF);
        header[2] = static_cast<uint8_t>((coordCount_ >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>((coordCount_ >> 16) & 0xFF);
        header[4] = static_cast<uint8_t>((coordCount_ >> 24) & 0xFF);
        header[5] = static_cast<uint8_t>(s & 0xFF);
        header[6] = static_cast<uint8_t>(s >> 8);

        broadcaster_->beginBinaryFrame(sizeof(header) + static_cast<size_t>(coordCount_) * 3);
        broadcaster_->pushBinaryFrame(header, sizeof(header));

        if (s == 1 && cpl == 3 && coordCount_ <= n) {
            // FULL RES, RGB: the producer buffer IS the payload — push it 1:1, no copy, no walk.
            // The common case (any grid ≤ cap, incl. 16K on a no-PSRAM classic): zero buffers.
            broadcaster_->pushBinaryFrame(src, static_cast<size_t>(coordCount_) * 3);
        } else {
            // Downsampled (s>1) or non-RGB (cpl≠3): walk forEachCoord with the SAME lattice skip
            // the coord table used — same subset, same order, so colour[k] ↔ coord[k] line up
            // with no stored index map. Push 3 bytes/light through a small stack scratch (the RGB
            // is read straight from the producer buffer at the light's driver index).
            struct ColCtx {
                mm::BinaryBroadcaster* bc; const uint8_t* src; nrOfLightsType n, s; uint8_t cpl;
                uint8_t buf[1536]; uint16_t fill;
            };
            // s is the FULL lattice stride (not clamped): the colour pass must use the SAME
            // predicate as buildAndSendCoordTable's, else above stride 255 the two disagree and
            // colour[k] no longer lines up with coord[k] (browser drops the mismatched frame).
            ColCtx col{broadcaster_, src, n, s, cpl, {}, 0};
            layer_->layouts()->forEachCoord([](void* c, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
                auto* p = static_cast<ColCtx*>(c);
                if (x % p->s != 0 || y % p->s != 0 || z % p->s != 0) return;
                const uint8_t* px = (idx < p->n) ? p->src + static_cast<size_t>(idx) * p->cpl : nullptr;
                p->buf[p->fill++] = px ? px[0] : 0;
                p->buf[p->fill++] = (px && p->cpl >= 2) ? px[1] : 0;
                p->buf[p->fill++] = (px && p->cpl >= 3) ? px[2] : 0;
                if (p->fill > sizeof(p->buf) - 3) { p->bc->pushBinaryFrame(p->buf, p->fill); p->fill = 0; }
            }, &col);
            if (col.fill) broadcaster_->pushBinaryFrame(col.buf, col.fill);
        }
        return broadcaster_->endBinaryFrame();
    }

private:
    // Frame cap: the most points one preview frame carries before the spatial-lattice
    // downsample engages. There is no per-frame buffer — the colour frame streams straight from
    // the producer buffer (and the coord table from forEachCoord) through the broadcaster's
    // beginBinaryFrame/pushBinaryFrame/endBinaryFrame, so the cap isn't a buffer size but the
    // point count the device can stream comfortably without the per-frame work and wire bytes
    // dominating its loop. PSRAM boards stream far more points than a no-PSRAM classic; two
    // compile-time tiers off platform::hasPsram, with the spatial-lattice downsample as the
    // graceful fallback above the cap. Tune against the per-board live sweep (the break point
    // each board actually hits). The literals are split so each fits its board's nrOfLightsType
    // (u16 on classic, u32 on PSRAM) — a single ternary would force both constants through the
    // u16 type on a classic build and overflow.
    static constexpr nrOfLightsType MAX_PREVIEW_POINTS =
        platform::hasPsram ? static_cast<nrOfLightsType>(131072u)   // PSRAM: 128K pts (384 KB) into PSRAM
                           : static_cast<nrOfLightsType>(16384u);   // classic: ~16K pts (48 KB) internal RAM

    // Map an axis coordinate into the 0..255 byte range. posScale_ == 0 means
    // the box already fits (1:1, exact integer positions); otherwise scale by
    // 255/posScale_ (posScale_ = the largest box edge), preserving aspect ratio
    // so a >255 axis isn't silently flattened onto the 255 plane.
    uint8_t scaleAxis(lengthType v) const {
        if (v < 0) return 0;
        int32_t s = posScale_ ? (static_cast<int32_t>(v) * 255 / posScale_) : v;
        return s > 255 ? 255 : static_cast<uint8_t>(s);
    }

    Buffer* sourceBuffer_ = nullptr;
    BinaryBroadcaster* broadcaster_ = nullptr;
    nrOfLightsType coordCount_ = 0;        // lights the lattice keeps = the streamed 0x03/0x02 count
    nrOfLightsType previewStride_ = 1;     // wire field: the lattice/downscale factor (1 = full res)
    bool coordPending_ = false;            // coord table not yet delivered; loop() retries it
    uint8_t bx_ = 0, by_ = 0, bz_ = 0;
    int32_t posScale_ = 0;            // 0 = positions 1:1; else largest box edge (>255) to scale by
    uint32_t lastSendTime_ = 0;
    uint32_t lastClientGen_ = 0;   // last seen broadcaster_->clientGeneration() — re-send coords on change

    // Adaptive downscaling. The preview streams at the finest resolution the link sustains.
    // The streamed send is all-or-nothing per client, so a frame (colour or coord table) that
    // doesn't reach every client means the link can't keep up at this resolution: coarsen
    // (downscale_++) after a short run of such frames so the rebuilt lattice sends fewer points.
    // A sustained run of fully-sent frames refines back toward full resolution (downscale_--).
    // downscale_ is an extra floor on the per-axis lattice stride, composing with the cap
    // downsample; it rides the wire stride field to the browser's "preview 1/N · link limited"
    // status. (≥1; 1 = full resolution.) Hysteresis via the streak thresholds stops oscillation.
    nrOfLightsType downscale_ = 1;
    uint8_t slowStreak_ = 0;       // consecutive frames the link couldn't fully send
    uint8_t cleanStreak_ = 0;      // consecutive fully-sent frames
    static constexpr uint8_t kDownscaleAfterSlow = 4;    // coarsen after this many struggling frames
    static constexpr uint8_t kUpscaleAfterFast   = 20;   // refine after this many clean frames
};

} // namespace mm
