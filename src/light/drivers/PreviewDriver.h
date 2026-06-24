#pragma once

#include "light/drivers/Drivers.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType
#include "core/BinaryBroadcaster.h"
#include "platform/platform.h"

#include <limits>  // numeric_limits for the memory-derived point cap

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
        // A resize frees+reallocs the producer buffer, so any in-flight resumable colour send holds
        // a pointer that's about to dangle — cancel it BEFORE the rebuild (the browser discards the
        // half-sent message and gets the fresh table + frame next tick). Guards a use-after-free.
        if (broadcaster_) broadcaster_->cancelBufferedSend();
        buildAndSendCoordTable();
        MoonModule::onBuildState();
    }

    void loop() override {
        if (fps == 0) return;
        uint32_t now = platform::millis();
        uint32_t interval = 1000 / fps;
        if (now - lastSendTime_ < interval) return;  // fps CEILING (max rate); link may be slower
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

        // ADAPTIVE FRAME RATE. The full-res colour frame streams resumably (sendBufferedFrame drains
        // across transport ticks), so a frame only starts once the previous one fully drained. We
        // gate on that: idle → send the next frame now; still draining → skip this slot. The
        // EFFECTIVE fps therefore self-limits to what the link sustains — fast links hit the fps
        // ceiling, slow links naturally drop to a few fps, with NO loop stall either way. The slot
        // we skip is also the "link is slow" signal (framesWaiting_), so we shed frame rate FIRST.
        bool frameOk = true;
        bool sentThisSlot = false;
        bool sentFrameWasSlow = false;
        if (!coordPending_) {
            const bool idle = !broadcaster_ || broadcaster_->bufferedSendIdle();
            if (idle) {
                // The previous frame finished draining. How many fps slots did it take? > a couple
                // means the link can't sustain this resolution at the requested rate — that frame
                // was "slow", the resolution signal below.
                sentFrameWasSlow = framesWaiting_ >= kSlowFrames;
                frameOk = sendFrame();          // false → a client couldn't take the frame (closed)
                sentThisSlot = true;
                framesWaiting_ = 0;
            } else {
                if (framesWaiting_ < 255) framesWaiting_++;  // still draining — link behind (saturate, no wrap)
            }
        }

        // ADAPTIVE RESOLUTION (the deeper fallback, after frame rate). The struggle signal is
        // LATENCY: the just-completed frame took more than kSlowFrames slots to drain
        // (sentFrameWasSlow), or a frame/coord table didn't reach a client. This fires even when
        // frames eventually send (the slow-but-complete case a pure all-sent signal misses — a
        // full-res 128² frame that delivers at ~2 fps). On a sustained run of slow frames, coarsen
        // the lattice (downscale_++) so frames shrink and the rate climbs; a sustained run of
        // prompt, fully-sent frames refines back toward full res (downscale_--). The streaks only
        // advance on slots where a frame completed (sentThisSlot), so a long drain counts as ONE
        // slow frame, not many — making kDownscaleAfterSlow a count of slow frames, not ticks.
        // Hysteresis stops oscillation; the factor rides the wire stride field to the status line.
        const bool linkStruggling =
            coordPending_ || (sentThisSlot && (!frameOk || sentFrameWasSlow));
        if (linkStruggling) {
            cleanStreak_ = 0;
            if (++slowStreak_ >= kDownscaleAfterSlow && downscale_ < 64) {
                slowStreak_ = 0;
                downscale_++;
                buildAndSendCoordTable();
            }
        } else if (sentThisSlot) {   // only count a clean run on slots where we actually sent
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

        // Box EXTENT = the maximum coordinate the positions reach, which is (size − 1): forEachCoord
        // emits x in [0, width−1], so an 8-wide grid spans 0..7 and its extent is 7, NOT 8. The
        // header carries these extents and the browser centres the cloud by dividing by the largest,
        // so they must match the packed coordinates' span exactly — using the size (8) instead drew
        // the wireframe box one cell too large and shifted the lights off-centre.
        auto extent = [](lengthType size) -> lengthType { return size > 0 ? size - 1 : 0; };
        const lengthType ex = extent(layer_->physicalWidth());
        const lengthType ey = extent(layer_->physicalHeight());
        const lengthType ez = extent(layer_->physicalDepth());
        // Positions are 1 byte/axis. To support layouts whose extent exceeds 255 on an axis (a
        // 512-wide grid, say), scale every axis by the same factor so the largest edge maps to 255 —
        // preserving aspect ratio. For extents ≤255/axis the factor is 1 (exact integer positions).
        lengthType maxEdge = ex;
        if (ey > maxEdge) maxEdge = ey;
        if (ez > maxEdge) maxEdge = ez;
        if (maxEdge < 1) maxEdge = 1;
        posScale_ = (maxEdge > 255) ? maxEdge : 0;   // 0 = no scaling (1:1)
        bx_ = scaleAxis(ex);
        by_ = scaleAxis(ey);
        bz_ = scaleAxis(ez);

        // Per-axis downsample step s (lattice skip x%s && y%s && z%s). The cell count of the
        // bounding box is the upper bound on kept lights, so grow s until it fits the cap — but
        // ONLY when the layout has more lights than the cap (a sparse layout — big box, few
        // lights — fits at s==1 and must not be downsampled for its box size alone). The wire
        // "stride" field carries s to the browser (1 = full res; >1 = "1/s shown, link limited").
        const lengthType ax = layer_->physicalWidth()  > 0 ? layer_->physicalWidth()  : 1;
        const lengthType ay = layer_->physicalHeight() > 0 ? layer_->physicalHeight() : 1;
        const lengthType az = layer_->physicalDepth()  > 0 ? layer_->physicalDepth()  : 1;
        nrOfLightsType s = 1;
        const nrOfLightsType cap = maxPreviewPoints();   // memory-derived this rebuild
        if (n > cap) {
            auto latticeCount = [&](nrOfLightsType step) {
                nrOfLightsType cx = (ax + step - 1) / step, cy = (ay + step - 1) / step,
                               cz = (az + step - 1) / step;
                return static_cast<uint32_t>(cx) * cy * cz;
            };
            while (latticeCount(s) > cap) s++;
        }
        if (s < downscale_) s = downscale_;   // adaptive: never finer than the link sustains
        previewStride_ = s;

        // Count the lights the lattice keeps. A dense grid in natural order (no LUT) is a regular
        // box, so the kept count is closed-form: ceil(size/s) per axis — no walk. A sparse/mapped
        // layout (LUT) has an arbitrary index↔position map, so it's counted by one forEachCoord
        // pass applying the same lattice predicate the colour/coord passes use (colour[k] ↔ coord[k]
        // line up by shared order, no stored index map).
        if (denseGrid()) {
            const nrOfLightsType cx = (ax + s - 1) / s, cy = (ay + s - 1) / s, cz = (az + s - 1) / s;
            coordCount_ = static_cast<nrOfLightsType>(static_cast<uint32_t>(cx) * cy * cz);
        } else {
            struct CountCtx { nrOfLightsType s, out; };
            CountCtx cc{s, 0};
            layouts->forEachCoord([](void* c, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
                auto* p = static_cast<CountCtx*>(c);
                if (x % p->s == 0 && y % p->s == 0 && z % p->s == 0) p->out++;
            }, &cc);
            coordCount_ = cc.out;
        }
        if (coordCount_ == 0) { coordPending_ = false; return; }

        // 0x03 app header: [type][count:u32 LE][bx][by][bz][stride:u16 LE] (10 bytes).
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
        // Push the kept lights' scaled positions in small slices through a stack scratch. A dense
        // grid strides its box directly (closed-form, no walk over skipped cells); a sparse/mapped
        // layout walks forEachCoord with the lattice predicate. BOTH visit the kept lights in the
        // SAME order the colour pass uses, so colour[k] ↔ coord[k] line up. The C callback can't
        // capture, so it shares PosCtx (used by both the dense loop and the sparse callback).
        struct PosCtx {
            PreviewDriver* self; mm::BinaryBroadcaster* bc; nrOfLightsType s;
            uint8_t buf[1536]; uint16_t fill;
            void emit(lengthType x, lengthType y, lengthType z) {
                buf[fill++] = self->scaleAxis(x);
                buf[fill++] = self->scaleAxis(y);
                buf[fill++] = self->scaleAxis(z);
                if (fill > sizeof(buf) - 3) { bc->pushBinaryFrame(buf, fill); fill = 0; }
            }
        };
        PosCtx pc{this, broadcaster_, s, {}, 0};
        if (denseGrid()) {
            for (lengthType z = 0; z < az; z += s)
                for (lengthType y = 0; y < ay; y += s)
                    for (lengthType x = 0; x < ax; x += s) pc.emit(x, y, z);
        } else {
            layouts->forEachCoord([](void* c, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
                auto* p = static_cast<PosCtx*>(c);
                if (x % p->s != 0 || y % p->s != 0 || z % p->s != 0) return;
                p->emit(x, y, z);
            }, &pc);
        }
        if (pc.fill) broadcaster_->pushBinaryFrame(pc.buf, pc.fill);
        // The coord table must reach the browser before colour frames carrying the new count (the
        // browser skips a count-mismatched 0x02). endBinaryFrame() reports whether every client got
        // it; loop() retries while coordPending_ and withholds colour frames until it lands.
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

        if (s == 1 && cpl == 3 && coordCount_ <= n) {
            // FULL RES, RGB: the producer buffer IS the payload. Hand it to the RESUMABLE buffered
            // send (header copied, body = the producer buffer, a stable pointer) — it drains across
            // transport ticks without a copy and without spinning this loop, the fix for the
            // large-frame stall. The common case (any grid ≤ cap, incl. 16K on a no-PSRAM classic).
            // onBuildState cancels it before a resize frees the buffer (use-after-free guard).
            return broadcaster_->sendBufferedFrame(header, sizeof(header),
                                                   src, static_cast<size_t>(coordCount_) * 3);
        }

        // Downsampled (s>1) or non-RGB (cpl≠3): build the kept lights' colours into the synchronous
        // begin/push/end stream (no stable contiguous body for the resumable path). The kept subset
        // + order MUST match the coord table's, so colour[k] ↔ coord[k] line up (the browser drops a
        // count/stride-mismatched frame). A dense grid strides its box directly — light (x,y,z) is at
        // buffer index z·H·W + y·W + x, closed-form, no walk over skipped cells (this is the cost the
        // forEachCoord walk used to pay every frame). A sparse/mapped layout walks forEachCoord with
        // the same lattice predicate (its index↔position map is arbitrary — no formula).
        broadcaster_->beginBinaryFrame(sizeof(header) + static_cast<size_t>(coordCount_) * 3);
        broadcaster_->pushBinaryFrame(header, sizeof(header));
        struct ColCtx {
            mm::BinaryBroadcaster* bc; const uint8_t* src; nrOfLightsType n; uint8_t cpl;
            uint8_t buf[1536]; uint16_t fill;
            void emit(nrOfLightsType idx) {
                const uint8_t* px = (idx < n) ? src + static_cast<size_t>(idx) * cpl : nullptr;
                buf[fill++] = px ? px[0] : 0;
                buf[fill++] = (px && cpl >= 2) ? px[1] : 0;
                buf[fill++] = (px && cpl >= 3) ? px[2] : 0;
                if (fill > sizeof(buf) - 3) { bc->pushBinaryFrame(buf, fill); fill = 0; }
            }
        };
        ColCtx col{broadcaster_, src, n, cpl, {}, 0};
        if (denseGrid()) {
            const lengthType W = layer_->physicalWidth(), H = layer_->physicalHeight();
            const lengthType az = layer_->physicalDepth() > 0 ? layer_->physicalDepth() : 1;
            const lengthType ay = H > 0 ? H : 1, ax = W > 0 ? W : 1;
            for (lengthType z = 0; z < az; z += s)
                for (lengthType y = 0; y < ay; y += s)
                    for (lengthType x = 0; x < ax; x += s)
                        col.emit(static_cast<nrOfLightsType>(static_cast<size_t>(z) * H * W
                                                             + static_cast<size_t>(y) * W + x));
        } else {
            // s as the FULL lattice stride (not clamped) — must match buildAndSendCoordTable's.
            struct Skip { ColCtx* col; nrOfLightsType s; } sk{&col, s};
            layer_->layouts()->forEachCoord([](void* c, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
                auto* p = static_cast<Skip*>(c);
                if (x % p->s != 0 || y % p->s != 0 || z % p->s != 0) return;
                p->col->emit(idx);
            }, &sk);
        }
        if (col.fill) broadcaster_->pushBinaryFrame(col.buf, col.fill);
        return broadcaster_->endBinaryFrame();
    }

private:
    // Frame cap: the most points one preview frame carries before the spatial-lattice downsample
    // engages — derived at runtime from free contiguous memory, not a fixed per-board constant
    // (architecture.md § Scaling to available memory: "sizes determined at runtime based on
    // available memory"). There is no per-frame buffer; the cap bounds the transient work the coord
    // table build (3 bytes/point in flight to the socket) and the resumable colour send impose. So
    // a fragmented classic downscales SOONER (less contiguous RAM) while a roomy PSRAM board goes
    // far higher — one rule, every board, measured not assumed. The spatial-lattice downsample is
    // the graceful fallback above the cap.
    // True when the source is a dense grid in natural box order (no mapping LUT): driver index i is
    // exactly box cell i, so the kept-light set + each light's buffer index are CLOSED-FORM from the
    // box dimensions and the stride — no forEachCoord walk needed (the count, the coord positions,
    // and the downsampled colours all stride the box directly). A LUT means a sparse / serpentine /
    // modified layout whose index↔position map is arbitrary, so those paths must walk forEachCoord.
    // Mirrors the Layer's own dense-vs-LUT decision (Layer::isNaturalOrder gates lut_.setIdentity),
    // so the two agree: no LUT ⇔ Drivers passed the dense box buffer ⇔ closed-form is valid here.
    bool denseGrid() const { return layer_ && !layer_->lut().hasLUT(); }

    nrOfLightsType maxPreviewPoints() const {
        // TWO independent bounds, take the smaller:
        //  (1) DISPLAY cap — a preview is a browser canvas a few hundred px wide; beyond ~4096
        //      points the lights are sub-pixel and indistinguishable, so MORE points only cost link
        //      bandwidth (a 16K-point 49 KB frame streams at <1 fps even on Ethernet). Capping to a
        //      display-sensible count is what makes a big-RAM board (P4) downsample to a frame the
        //      LINK can actually push fast — the bottleneck here is throughput, not memory. WLED-MM
        //      caps its live preview the same way. The lattice downsample (and the browser's status)
        //      handle anything larger gracefully.
        //  (2) MEMORY cap — derived from maxAllocBlock() so a tight/fragmented board downsamples even
        //      SOONER than the display cap (architecture.md § Scaling to available memory).
        // min(display, memory): the display cap normally wins (it's the smaller); the memory cap
        // only bites on a board too tight to stream even 4096 points.
        constexpr uint32_t kDisplayCap = 4096;       // visual-resolution ceiling for ANY board
        constexpr size_t kReserve = 32u * 1024u;     // leave this much contiguous headroom
        constexpr size_t kBytesPerPoint = 3u;        // RGB on the wire / position bytes in the table
        constexpr nrOfLightsType kFloor = 1024;      // always previewable (hard-downsampled) on any board
        const size_t block = platform::maxAllocBlock();
        // maxAllocBlock() returns 0 = "unlimited / not reported" (desktop, test default): memory is
        // not the limit there, so the display cap governs.
        uint32_t memPts;
        if (block == 0) {
            memPts = kDisplayCap;
        } else {
            const size_t usable = block > kReserve ? block - kReserve : 0;
            memPts = static_cast<uint32_t>(usable / kBytesPerPoint);
            if (memPts < kFloor) memPts = kFloor;
        }
        uint32_t pts = memPts < kDisplayCap ? memPts : kDisplayCap;
        // Clamp into the board's nrOfLightsType range (u16 on a no-PSRAM classic).
        constexpr uint32_t kTypeMax = static_cast<uint32_t>(std::numeric_limits<nrOfLightsType>::max());
        if (pts > kTypeMax) pts = kTypeMax;
        return static_cast<nrOfLightsType>(pts);
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
    uint8_t slowStreak_ = 0;       // consecutive struggling frames (latency or not-all-sent)
    uint8_t cleanStreak_ = 0;      // consecutive prompt, fully-sent frames
    uint8_t framesWaiting_ = 0;    // fps slots skipped because the previous frame is still draining
    static constexpr uint8_t kDownscaleAfterSlow = 2;    // coarsen after this many slow frames (fast react)
    static constexpr uint8_t kUpscaleAfterFast   = 20;   // refine after this many clean frames
    // A frame still draining after this many fps slots means the link can't sustain even one frame
    // at this resolution at the slowest useful rate → resolution must drop (not just the rate). Set
    // above 1 so a normal multi-tick drain on a healthy link isn't mistaken for struggle.
    static constexpr uint8_t kSlowFrames = 3;
};

} // namespace mm
