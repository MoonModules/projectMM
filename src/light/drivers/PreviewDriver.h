#pragma once

#include "light/drivers/Drivers.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType
#include "core/BinaryBroadcaster.h"
#include "platform/platform.h"
#include <new>                  // std::nothrow

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

        // The coordinate table is sent only when the geometry actually changes
        // (onBuildState — a grid resize, layout/LUT rebuild) or when the UI asks for it
        // (a new WS client bumps the broadcaster's clientGeneration, so a page refresh
        // gets the positions immediately). NOT per-frame and NOT on a timer: rebuilding
        // the full table every tick would starve the render loop, and the colour frames
        // below already reference the last-sent positions. coordCount_==0 covers the cold
        // case where the layout wasn't wired yet at onBuildState time.
        uint32_t gen = broadcaster_ ? broadcaster_->clientGeneration() : 0;
        if (coordCount_ == 0 || gen != lastClientGen_) {
            lastClientGen_ = gen;
            buildAndSendCoordTable();   // sets coordPending_ if the 0x03 was dropped
        } else if (coordPending_) {
            // A previous coord table was dropped under backpressure. Retry it (no rebuild —
            // the geometry hasn't changed, just re-broadcast the same bytes) until it lands.
            coordPending_ = !sendCoordTable();
        }

        // Hold colour frames until the browser has the matching coordinate table: a 0x02 with
        // the new count plotted against the old coords would be skipped by the browser's
        // count-mismatch guard, and if the 0x03 stays dropped that would freeze the preview.
        if (!coordPending_) sendFrame();

        // Adaptive downscaling, driven by DRAIN LATENCY (not dropped frames). A dropped frame
        // is normal — at fps=24 the producer naturally outruns the per-frame socket drain even
        // on a fast link, so "frame dropped" over-triggers. The real signal is how many
        // transport-poll ticks the last frame took to fully send: 1-2 ticks = the link keeps
        // up; many ticks = genuinely backpressured. Coarsen (downscale_++) when latency is high
        // for a sustained run; refine (downscale_--) when it's been low. Hysteresis via the
        // streaks stops oscillation. A change rebuilds the coordinate table (the lattice
        // changed) and re-primes the browser, whose status line shows the new factor. Skip the
        // adaptation while a coord table is still pending — don't stack another rebuild on top.
        const uint16_t drainTicks = broadcaster_ ? broadcaster_->lastDrainTicks() : 1;
        if (coordPending_) {
            // pending coord table: don't change the factor until it lands
        } else if (drainTicks > kDrainTicksHigh) {
            cleanStreak_ = 0;
            if (++slowStreak_ >= kDownscaleAfterSlow && downscale_ < 64) {
                slowStreak_ = 0;
                downscale_++;
                buildAndSendCoordTable();
            }
        } else if (drainTicks <= kDrainTicksLow) {
            slowStreak_ = 0;
            if (downscale_ > 1 && ++cleanStreak_ >= kUpscaleAfterFast) {
                cleanStreak_ = 0;
                downscale_--;
                buildAndSendCoordTable();
            }
        } else {
            // Mid-range latency: stable, hold the current factor (don't drift either way).
            slowStreak_ = 0;
            cleanStreak_ = 0;
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

        // Downsample SPATIALLY, not by flat index. Picking every Nth light in driver
        // order moirés on a 2D grid (the sampled column drifts each row when N doesn't
        // divide the width → diagonal blank streaks). Instead, keep a light only when its
        // grid position falls on a coarse lattice (qx%sx==0 && qy%sy==0 && qz%sz==0): a
        // regular sub-grid, no drift — and it generalises to 3D (cube, sphere) since the
        // lattice is per-axis on the real coordinates, not the index. sx/sy/sz are chosen
        // so the kept count fits MAX_PREVIEW_POINTS. The kept lights' DRIVER indices are
        // recorded in sampledIdx_ so the per-frame colour pass sends the SAME lights in the
        // SAME order (lockstep); the wire stride is 1 (the browser maps colour k → coord k).
        const lengthType ax = layer_->physicalWidth()  > 0 ? layer_->physicalWidth()  : 1;
        const lengthType ay = layer_->physicalHeight() > 0 ? layer_->physicalHeight() : 1;
        const lengthType az = layer_->physicalDepth()  > 0 ? layer_->physicalDepth()  : 1;
        nrOfLightsType sx = 1, sy = 1, sz = 1;
        // Grow the per-axis lattice stride uniformly until the lattice-point count fits.
        // (Active axes only — a flat 2D grid leaves sz at 1.)
        auto latticeCount = [&](nrOfLightsType s) {
            nrOfLightsType cx = (ax + s - 1) / s, cy = (ay + s - 1) / s, cz = (az + s - 1) / s;
            return static_cast<uint32_t>(cx) * cy * cz;
        };
        nrOfLightsType s = 1;
        while (latticeCount(s) > MAX_PREVIEW_POINTS) s++;
        if (s < downscale_) s = downscale_;   // adaptive: never finer than the link sustains
        sx = sy = sz = s;

        // Buffers sized to the points we'll actually send — min(n, cap) — not the full cap:
        // an 8×8 grid uses 192 B, not 65 KB. A grid ≤ ~145² sends every light (s==1); only a
        // bigger one downsamples (s>1) and the lattice count is the upper bound. coords_/rgb_
        // are PSRAM-backed (platform::alloc); sampledIdx_ is the index list.
        const nrOfLightsType sendCap = n < MAX_PREVIEW_POINTS ? n : MAX_PREVIEW_POINTS;
        if (!coords_.data() || coords_.count() < sendCap) {
            coords_.allocate(sendCap, 3);  // owned u8×3 position buffer
        }
        if (!sampledIdx_ || sampledIdxCap_ < sendCap) {
            delete[] sampledIdx_;
            // nothrow so the !sampledIdx_ guard below actually catches OOM — plain new aborts
            // on the ESP32, which would crash the device instead of degrading the preview.
            sampledIdx_ = new (std::nothrow) nrOfLightsType[sendCap];
            sampledIdxCap_ = sampledIdx_ ? sendCap : 0;
        }
        if (!coords_.data() || !sampledIdx_) { coordCount_ = 0; coordPending_ = false; return; }

        struct PackCtx {
            PreviewDriver* self; uint8_t* dst; nrOfLightsType* idxOut;
            nrOfLightsType sx, sy, sz, out, cap;
        };
        PackCtx pc{this, coords_.data(), sampledIdx_, sx, sy, sz, 0, sendCap};
        layouts->forEachCoord([](void* c, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
            auto* p = static_cast<PackCtx*>(c);
            // Keep only lattice points — a regular spatial sub-sample (works in 2D and 3D).
            if (x % p->sx != 0 || y % p->sy != 0 || z % p->sz != 0) return;
            if (p->out >= p->cap) return;
            uint8_t* d = p->dst + static_cast<size_t>(p->out) * 3;
            d[0] = p->self->scaleAxis(x); d[1] = p->self->scaleAxis(y); d[2] = p->self->scaleAxis(z);
            p->idxOut[p->out] = idx;       // driver index → colour pass reads the same lights
            p->out++;
        }, &pc);
        coordCount_ = pc.out;
        // The wire "stride" field now carries the effective DOWNSCALE factor (per axis) for the
        // browser's status line — colour k still maps 1:1 to coord k (the index list picks the
        // lights). 1 = full resolution; >1 = "showing 1/s of the lights, link can't keep up".
        stride_ = s;

        // Header: [0x03][count:u32 LE][bx][by][bz][stride:u16 LE]  (10 bytes)
        uint8_t* h = coordHeader_;
        h[0] = 0x03;
        h[1] = static_cast<uint8_t>(coordCount_ & 0xFF);
        h[2] = static_cast<uint8_t>((coordCount_ >> 8) & 0xFF);
        h[3] = static_cast<uint8_t>((coordCount_ >> 16) & 0xFF);
        h[4] = static_cast<uint8_t>((coordCount_ >> 24) & 0xFF);
        h[5] = bx_; h[6] = by_; h[7] = bz_;
        h[8] = static_cast<uint8_t>(stride_ & 0xFF);
        h[9] = static_cast<uint8_t>(stride_ >> 8);

        // The coordinate table MUST reach the browser before colour frames that carry the new
        // count — else the browser's count-mismatch guard skips every colour frame and the
        // preview freezes. broadcastBinary can DROP the 0x03 under backpressure (a colour frame
        // still draining), so track whether it landed; loop() retries while pending and holds
        // off colour frames until it lands.
        coordPending_ = !sendCoordTable();
    }

    // Produce + push one per-frame 0x02 RGB message. Returns the broadcaster's accept/drop
    // result (false = dropped under backpressure) so loop() can drive adaptive downscaling.
    // Public so tests can drive it without the loop() rate-limit.
    bool sendFrame() {
        if (!broadcaster_ || !sourceBuffer_ || !sourceBuffer_->data() || coordCount_ == 0) return false;
        const uint8_t* src = sourceBuffer_->data();
        uint8_t cpl = sourceBuffer_->channelsPerLight();
        nrOfLightsType n = sourceBuffer_->count();

        // RGB for exactly the lights the coordinate table sampled, in the same order —
        // sampledIdx_[k] is the driver index of sent point k (built in buildAndSendCoordTable
        // by the spatial lattice). Iterating that list keeps colour ↔ position in lockstep
        // regardless of how the sampling chose them.
        if (!rgb_.data() || rgb_.count() < coordCount_) rgb_.allocate(coordCount_, 3);
        if (!rgb_.data() || !sampledIdx_) return false;
        uint8_t* dst = rgb_.data();
        nrOfLightsType out = 0;
        for (nrOfLightsType k = 0; k < coordCount_; k++) {
            nrOfLightsType i = sampledIdx_[k];
            if (i >= n) continue;                  // layout shrank since the table was built
            const uint8_t* s = src + static_cast<size_t>(i) * cpl;
            dst[out * 3 + 0] = s[0];
            dst[out * 3 + 1] = cpl >= 2 ? s[1] : 0;
            dst[out * 3 + 2] = cpl >= 3 ? s[2] : 0;
            out++;
        }

        // Header: [0x02][count:u32 LE][stride:u16 LE]  (7 bytes)
        uint8_t header[7];
        header[0] = 0x02;
        header[1] = static_cast<uint8_t>(out & 0xFF);
        header[2] = static_cast<uint8_t>((out >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>((out >> 16) & 0xFF);
        header[4] = static_cast<uint8_t>((out >> 24) & 0xFF);
        header[5] = static_cast<uint8_t>(stride_ & 0xFF);
        header[6] = static_cast<uint8_t>(stride_ >> 8);

        const platform::WriteChunk payload[] = {
            { header, sizeof(header) },
            { dst, static_cast<size_t>(out) * 3 },
        };
        return broadcaster_->broadcastBinary(payload, 2);
    }

private:
    // Frame cap: the most points one preview frame carries before the spatial-lattice
    // downsample engages. The old ~1800 limit was the single-writev wall; that's gone now —
    // broadcastBinary enqueues the frame and HttpServerModule::drainWsSends() streams it
    // across loop20ms ticks (non-blocking), so the cap is no longer "what fits one writev"
    // but "how big a frame the device can stage + stream comfortably". That's RAM-bound:
    // the staging frame is points×3 bytes (16K LEDs = 48 KB), so a no-PSRAM classic board
    // tops out around 16K in internal RAM while PSRAM boards go far higher. Two compile-time
    // tiers off platform::hasPsram; downsampling stays as the graceful fallback above the cap.
    // Tune these against the per-board live sweep (the break point each board actually hits).
    // The literals are split so each fits its board's nrOfLightsType (u16 on classic, u32 on
    // PSRAM) — a single ternary would force both constants through the u16 type on a classic
    // build and overflow.
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

    // Returns whether the broadcaster accepted the table (false = dropped under backpressure).
    bool sendCoordTable() {
        if (!broadcaster_ || coordCount_ == 0 || !coords_.data()) return false;
        const platform::WriteChunk payload[] = {
            { coordHeader_, sizeof(coordHeader_) },
            { coords_.data(), static_cast<size_t>(coordCount_) * 3 },
        };
        return broadcaster_->broadcastBinary(payload, 2);
    }

    Buffer* sourceBuffer_ = nullptr;
    BinaryBroadcaster* broadcaster_ = nullptr;
    Buffer coords_;               // owned; u8×3 positions, one per sent point
    Buffer rgb_;                  // owned; u8×3 colours, one per sent point
    uint8_t coordHeader_[10] = {};   // [0x03][count:u32][bx][by][bz][stride:u16]
    nrOfLightsType coordCount_ = 0;   // points actually sent
    nrOfLightsType stride_ = 1;       // wire field: the lattice/downscale factor (1 = full res)
    bool coordPending_ = false;       // a coord table was dropped under backpressure; loop() retries it
    // Driver indices of the sampled lights (sent point k = driver light sampledIdx_[k]).
    // Built in buildAndSendCoordTable, read by sendFrame so colour ↔ position stay locked.
    // Raw heap (not Buffer) because it holds indices, not the u8×3 the Buffer helper packs.
    nrOfLightsType* sampledIdx_ = nullptr;
    nrOfLightsType sampledIdxCap_ = 0;
    uint8_t bx_ = 0, by_ = 0, bz_ = 0;
    int32_t posScale_ = 0;            // 0 = positions 1:1; else largest box edge (>255) to scale by
    uint32_t lastSendTime_ = 0;
    uint32_t lastClientGen_ = 0;   // last seen broadcaster_->clientGeneration() — re-send coords on change

    // Adaptive downscaling. The preview streams at the finest resolution the WS link sustains;
    // when a frame takes too many transport ticks to drain (high latency) we coarsen the
    // lattice (downscale_++) so frames shrink and catch up. A low-latency stretch steps it
    // back toward full resolution. Hysteresis (streak thresholds) stops oscillation. downscale_
    // is an extra floor on the per-axis lattice stride, so it composes with the RAM-cap
    // downsample. It rides the coord header's stride field to the browser, which shows
    // "preview 1/N · link limited" while > 1. (kept ≥1; 1 = full resolution / no downscale.)
    nrOfLightsType downscale_ = 1;
    uint8_t slowStreak_ = 0;       // consecutive HIGH-latency frames
    uint8_t cleanStreak_ = 0;      // consecutive LOW-latency frames
    // Latency thresholds in transport-poll ticks (~20 ms each). A frame that drains in ≤2 ticks
    // means the link has headroom; >4 ticks means it's struggling. The streak counts give
    // hysteresis: coarsen after a short slow run, refine after a longer fast run (slower to
    // refine so it doesn't flap right back into trouble).
    static constexpr uint16_t kDrainTicksLow  = 2;
    static constexpr uint16_t kDrainTicksHigh = 4;
    static constexpr uint8_t  kDownscaleAfterSlow = 4;    // coarsen after this many slow frames
    static constexpr uint8_t  kUpscaleAfterFast   = 20;   // refine after this many fast frames

public:
    ~PreviewDriver() override { delete[] sampledIdx_; }
};

} // namespace mm
