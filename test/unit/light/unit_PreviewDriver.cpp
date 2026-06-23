// @module PreviewDriver

#include "doctest.h"
#include "core/Scheduler.h"
#include "light/drivers/PreviewDriver.h"
#include "light/drivers/Drivers.h"
#include "light/layers/Layer.h"
#include "light/layers/Layers.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/SphereLayout.h"

#include <vector>
#include <cstring>

// PreviewDriver streams a true-shape point list: a one-time 0x03 coordinate
// table (positions of the real lights) + per-frame 0x02 RGB indexed by light.
// These tests pin: the table carries exactly lightCount positions (sphere → its
// shell count, NOT the bounding box), the per-frame RGB count matches, and a
// large layout is index-downsampled (stride > 1) to fit the send-buffer cap.

namespace {

// Captures the two preview message types so tests can inspect them. PreviewDriver STREAMS each
// frame via begin/push/end (no frame buffer); the mock reassembles the pushed slices, strips the
// WS header (begin is given the PAYLOAD length, so what's pushed is exactly the payload), and
// classifies by first byte at end. dropCoord/acceptNext make endBinaryFrame report a client that
// didn't get the whole frame (false) to drive the coord-pending retry + adaptive-downscale paths.
struct CaptureBroadcaster : mm::BinaryBroadcaster {
    int coordMsgs = 0, frameMsgs = 0;
    std::vector<uint8_t> lastCoord, lastFrame;
    std::vector<uint8_t> cur_;     // payload accumulated across pushBinaryFrame between begin/end
    uint32_t generation = 0;       // bump to simulate a new client connecting
    bool acceptNext = true;        // false → endBinaryFrame reports a colour frame not fully sent
    bool dropCoord = false;        // true → endBinaryFrame reports a coord table not fully sent

    void beginBinaryFrame(size_t /*totalLen*/) override { cur_.clear(); }
    void pushBinaryFrame(const uint8_t* data, size_t len) override {
        cur_.insert(cur_.end(), data, data + len);
    }
    bool endBinaryFrame() override {
        if (cur_.empty()) return false;
        const uint8_t type = cur_[0];
        if (type == 0x03) {
            if (dropCoord) return false;       // simulate the table not reaching the client
            coordMsgs++; lastCoord = cur_; return true;
        }
        if (type == 0x02) {
            if (!acceptNext) return false;     // simulate the colour frame not reaching the client
            frameMsgs++; lastFrame = cur_; return true;
        }
        return true;
    }
    uint32_t clientGeneration() const override { return generation; }

    // Resumable buffered send — the colour-frame path (coord table uses begin/push/end). The mock
    // captures it as a 0x02 frame (header ++ body). `bufferedDrains` models a slow link: the send
    // stays "in flight" for that many bufferedSendIdle() polls before going idle (0 = instant).
    // bufferedFrames counts accepted sends; bufferedDropped counts newest-wins backpressure drops.
    int bufferedFrames = 0, bufferedDropped = 0;
    int bufferedDrains = 0;            // ticks a send stays active (set >0 to model a slow link)
    int bufferedCanceled = 0;          // cancelBufferedSend() calls while a send was active
    const uint8_t* lastBody = nullptr; // body pointer of the in-flight send (for resize-safety test)
    bool sendBufferedFrame(const uint8_t* header, size_t headerLen,
                           const uint8_t* body, size_t bodyLen) override {
        if (active_) { bufferedDropped++; return false; }   // newest-wins backpressure
        if (!acceptNext) return false;
        bufferedFrames++; frameMsgs++;
        lastFrame.assign(header, header + headerLen);
        lastFrame.insert(lastFrame.end(), body, body + bodyLen);
        lastBody = body;
        remaining_ = bufferedDrains;       // >0 → stays "in flight" to model a slow link
        active_ = (remaining_ > 0);
        return true;
    }
    bool bufferedSendIdle() const override {
        if (active_ && remaining_ > 0) { --remaining_; if (remaining_ == 0) active_ = false; }
        return !active_;
    }
    void cancelBufferedSend() override { if (active_) bufferedCanceled++; active_ = false; }

    // 0x03 = [type][count:u32][bx][by][bz][stride:u16] (10-byte header)
    // 0x02 = [type][count:u32][stride:u16] (7-byte header)
    static uint32_t u32le(const std::vector<uint8_t>& b, size_t o) {
        return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
    }
    int coordCount() const { return lastCoord.size() >= 5 ? static_cast<int>(u32le(lastCoord, 1)) : -1; }
    int frameCount() const { return lastFrame.size() >= 5 ? static_cast<int>(u32le(lastFrame, 1)) : -1; }
    int coordStride() const { return lastCoord.size() >= 10 ? lastCoord[8] | (lastCoord[9] << 8) : -1; }

private:
    mutable bool active_ = false;     // a buffered send is in flight
    mutable int remaining_ = 0;       // bufferedSendIdle polls left before it goes idle
};

// Wire PreviewDriver under Drivers, over a Layer + single layout, with a
// CaptureBroadcaster — the full real path (sparse driver buffer + layout coords).
struct PreviewRig {
    mm::Layouts group;
    mm::Layer layer;
    mm::Drivers drivers;
    mm::PreviewDriver* preview;   // owned by drivers' child array
    CaptureBroadcaster cap;

    PreviewRig(mm::LayoutBase* layout, uint8_t cpl = 3) {
        group.addChild(layout);
        layer.setLayouts(&group);
        layer.setChannelsPerLight(cpl);
        layer.onBuildControls();
        layer.onBuildState();

        preview = new mm::PreviewDriver();
        preview->setBroadcaster(&cap);
        drivers.addChild(preview);
        drivers.setLayer(&layer);          // passBufferToDrivers wires preview's source + layer
        drivers.onBuildControls();
        drivers.onBuildState();
    }

    void produce() {
        preview->buildAndSendCoordTable();
        preview->sendFrame();
    }
};

} // namespace

// A sphere sends its SHELL lights (210), not the dense 9x9x9 box (729).
TEST_CASE("PreviewDriver coordinate table carries the real lights, not the box") {
    mm::SphereLayout s;
    s.radius = 4;                         // 210 shell lights, 9^3 box
    PreviewRig rig(&s);
    rig.produce();

    REQUIRE(rig.cap.coordMsgs > 0);
    CHECK(rig.cap.coordCount() == 210);   // the shell, not 729
    CHECK(rig.cap.coordStride() == 1);    // small → exact, no downsample
    // 0x03 = [0x03][count:u32][bx][by][bz][stride:u16] (10-byte hdr) + count*3 position bytes
    CHECK(rig.cap.lastCoord.size() == 10u + 210u * 3u);
}

// Per-frame 0x02 RGB count matches the coordinate-table count.
TEST_CASE("PreviewDriver per-frame RGB count matches the coordinate table") {
    mm::SphereLayout s;
    s.radius = 4;
    PreviewRig rig(&s);
    rig.produce();

    REQUIRE(rig.cap.frameMsgs > 0);
    CHECK(rig.cap.frameCount() == 210);
    // 0x02 = [0x02][count:u32][stride:u16] (7-byte hdr) + count*3 RGB bytes
    CHECK(rig.cap.lastFrame.size() == 7u + 210u * 3u);
}

// A small grid sends every light at its grid position (stride 1, exact).
TEST_CASE("PreviewDriver small grid sends all lights exactly") {
    mm::GridLayout g;
    g.width = 8; g.height = 8; g.depth = 1;   // 64 lights
    PreviewRig rig(&g);
    rig.produce();

    CHECK(rig.cap.coordCount() == 64);
    CHECK(rig.cap.frameCount() == 64);
    CHECK(rig.cap.coordStride() == 1);
}

// A large layout is SPATIALLY downsampled (a regular per-axis lattice, not every-Nth-flat-
// index) so the payload fits the send-buffer cap without the diagonal moiré that linear
// stride produced on a grid whose width didn't divide the stride. The wire "stride" field
// carries the per-axis lattice/downscale factor (colour k still maps 1:1 to coord k).
TEST_CASE("PreviewDriver downsamples a large layout on a regular spatial lattice") {
    // 200×200 = 40000 lights, over the 4096 display cap → the lattice downsample engages. The
    // extent (199) is ≤255/axis, so positions are sent at EXACT integer grid coordinates (no
    // byte-scaling rounding) — letting the regularity check below compare true lattice positions.
    mm::GridLayout g;
    g.width = 200; g.height = 200; g.depth = 1;
    PreviewRig rig(&g);
    rig.produce();

    CHECK(rig.cap.coordStride() >= 2);            // display cap forces a lattice step (the factor)
    CHECK(rig.cap.coordCount() <= 4096);          // downsampled under the display cap
    CHECK(rig.cap.coordCount() > 0);
    CHECK(rig.cap.coordCount() == rig.cap.frameCount());  // table + RGB agree (lockstep)

    // Regular lattice check: every sent X coordinate is a multiple of the same step, and so
    // is every Y — i.e. the kept points sit on a grid, with NO per-row column drift (the
    // diagonal-streak bug). Read the packed u8 positions back from the coord message.
    const auto& cd = rig.cap.lastCoord;
    const int hdr = 10;                           // [0x03][count:u32][bx][by][bz][stride:u16]
    REQUIRE(cd.size() >= static_cast<size_t>(hdr + 3));
    // Derive the X step from the first two distinct X values, then assert all X are multiples.
    int stepX = 0, x0 = cd[hdr];
    for (size_t p = hdr; p + 2 < cd.size(); p += 3) {
        int dx = cd[p] - x0;
        if (dx != 0) { stepX = dx > 0 ? dx : -dx; break; }
    }
    REQUIRE(stepX > 0);
    bool regular = true;
    for (size_t p = hdr; p + 2 < cd.size(); p += 3) {
        if (((cd[p] - x0) % stepX) != 0) { regular = false; break; }   // X off the lattice → drift
    }
    CHECK(regular);                               // no diagonal moiré
}

// A SPARSE layout under the cap must NOT be downsampled for its big BOUNDING BOX alone: the lattice
// bound is the layout's LIGHT count, not its box cell count, so a sphere whose shell fits the cap
// sends every light at stride 1 (a radius-8 sphere → ~812 shell lights, well under the 4096 display
// cap, in a 17³≈4913-cell box). (A genuinely huge sparse layout above the cap downsamples like any
// other — the cap is about points streamed, not box size.)
TEST_CASE("PreviewDriver keeps a sparse large-box layout at full resolution") {
    mm::SphereLayout s;
    s.radius = 8;                                 // big box (17³), shell light-count under the cap
    PreviewRig rig(&s);
    rig.produce();

    CHECK(rig.cap.coordCount() > 0);
    CHECK(rig.cap.coordCount() <= 4096);          // the shell fits the display cap...
    CHECK(rig.cap.coordStride() == 1);            // ...so it is sent whole, not downsampled
    CHECK(rig.cap.coordCount() == rig.cap.frameCount());
}

// Default fps is the rate-limited preview stream rate.
TEST_CASE("PreviewDriver fps default") {
    mm::PreviewDriver driver;
    CHECK(driver.fps == 24);
}

// Regression: a coordinate table dropped under backpressure must be RETRIED, and colour
// frames withheld until it lands — otherwise the device sends 0x02 frames the browser skips
// (count mismatch) and the preview freezes for the whole session. Drives loop() (where the
// coord-pending logic lives) with a broadcaster that drops every 0x03, then lets it through.
TEST_CASE("PreviewDriver retries a dropped coordinate table, withholds frames until it lands") {
    mm::GridLayout g; g.width = 16; g.height = 16; g.depth = 1;   // 256 lights, full res
    PreviewRig rig(&g);
    rig.cap.dropCoord = true;                 // every coord table is lost to backpressure
    rig.cap.frameMsgs = 0;                     // ignore any frame from rig construction
    rig.cap.generation = 1;                    // a "new client" — forces loop() to rebuild+resend
                                               // the coord table, which dropCoord now loses

    // Advance the test clock past the fps gate (interval = 1000/24 ≈ 42 ms) before each loop().
    uint32_t t = 1000;
    auto tick = [&] { t += 100; mm::platform::setTestNowMs(t); rig.preview->loop(); };

    // Pump loop() several times. The rebuilt 0x03 never lands, so NO colour frame may go out —
    // a 0x02 now would carry a count the browser can't map (the freeze the guard prevents).
    for (int i = 0; i < 5; i++) tick();
    CHECK(rig.cap.frameMsgs == 0);            // frames withheld while the table is pending

    // Link recovers: the table now lands, and frames resume — matching the same count.
    rig.cap.dropCoord = false;
    tick();                                    // retries the pending table (it lands)
    tick();                                    // now a colour frame may go out
    CHECK(rig.cap.coordMsgs > 0);              // the table finally reached the client
    CHECK(rig.cap.frameMsgs > 0);              // frames resumed
    CHECK(rig.cap.coordCount() == rig.cap.frameCount());   // and they agree (no freeze)

    mm::platform::setTestNowMs(0);             // release the clock override
}

// Regression: deleting the active Layer must not leave a driver holding a
// dangling layer_ pointer. Previously Drivers::passBufferToDrivers early-returned
// when the active Layer was null, leaving PreviewDriver's layer_ pointing at the
// freed Layer; the next onBuildState read layer_->layouts() on freed memory and
// crashed the device (LoadProhibited → boot loop, since the broken tree persists).
// Now passBufferToDrivers clears the drivers' layer_/sourceBuffer_ to null, a safe
// idle state. This drives the real path: Drivers bound to a Layers CONTAINER
// (self-healing), the Layer removed, then buildState re-resolves activeLayer()=null.
TEST_CASE("PreviewDriver tolerates the active Layer being deleted") {
    mm::GridLayout g; g.width = 16; g.height = 16; g.depth = 1;
    mm::Layouts group; group.addChild(&g);
    mm::Layers layers;
    auto* layer = new mm::Layer();
    layer->setChannelsPerLight(3);
    layers.addChild(layer);
    layers.setLayouts(&group);
    layers.onBuildControls();

    mm::Drivers drivers;
    auto* preview = new mm::PreviewDriver();
    CaptureBroadcaster cap;
    preview->setBroadcaster(&cap);
    drivers.addChild(preview);
    drivers.setLayers(&layers);          // container-bound: layer_ re-resolved at buildState
    drivers.onBuildControls();

    layers.onBuildState();
    drivers.onBuildState();
    REQUIRE(preview->layer() == layer);  // wired to the active Layer

    // Remove the only Layer, then rebuild — activeLayer() now returns null.
    layers.removeChild(layer);
    layer->teardown();
    mm::Scheduler::deleteTree(layer);    // free it — a stale pointer would now dangle

    layers.onBuildState();
    drivers.onBuildState();              // must NOT deref the freed Layer
    CHECK(preview->layer() == nullptr);  // cleared, not dangling

    // And producing a frame on the empty pipeline is a safe no-op (no crash).
    preview->buildAndSendCoordTable();
    preview->sendFrame();
    CHECK(cap.frameMsgs == 0);           // nothing to send with no layer
}

// Coordinates are sent ONLY when the geometry changes or a new client connects — never
// per-frame and never on a timer (a periodic full-table rebuild would starve the tick).
// A new client (clientGeneration bump) re-sends immediately so a page refresh shows the
// preview at once. Driven through loop() with a frozen clock for determinism.
TEST_CASE("PreviewDriver sends coordinates only on change / new client, never on a timer") {
    mm::platform::setTestNowMs(100000);
    PreviewRig rig(new mm::GridLayout(), 3);

    rig.preview->loop();                 // first loop: coords sent (count was 0)
    int afterFirst = rig.cap.coordMsgs;
    CHECK(afterFirst >= 1);

    // Advance a FULL 3 seconds with no new client and no rebuild: loop() keeps sending
    // colour frames but must NOT re-send the coordinate table. This is the regression
    // guard — the removed ~1 Hz timer would have re-sent ~3 times here.
    for (int t = 1; t <= 3; t++) {
        mm::platform::setTestNowMs(100000 + t * 1000);
        rig.preview->loop();
    }
    CHECK(rig.cap.coordMsgs == afterFirst);   // no timer-driven re-send across 3s

    // A new client connects (generation bumps). The next loop() re-sends coords at once.
    rig.cap.generation++;
    mm::platform::setTestNowMs(104200);
    rig.preview->loop();
    CHECK(rig.cap.coordMsgs == afterFirst + 1);   // re-sent for the fresh client

    mm::platform::setTestNowMs(0);       // restore the real clock for other tests
}

// A full-res RGB frame is sent through the RESUMABLE buffered path (sendBufferedFrame), whose body
// is the DRIVER (consumer) buffer itself — no copy. For a dense identity grid that's the Layer's
// dense box buffer; for a sparse/mapped layout it's the LUT-mapped output buffer (the real lights),
// the same buffer the LED drivers consume — NOT the dense box.
TEST_CASE("PreviewDriver routes a dense full-res frame through the resumable buffered send") {
    mm::GridLayout g; g.width = 16; g.height = 16; g.depth = 1;   // dense, no LUT (identity)
    PreviewRig rig(&g);
    rig.cap.bufferedFrames = 0;
    rig.produce();
    CHECK(rig.cap.bufferedFrames == 1);                      // went through sendBufferedFrame
    CHECK(rig.cap.frameCount() == 256);                      // the full grid, full res
    CHECK(rig.cap.frameCount() == rig.cap.coordCount());     // table + frame agree
    CHECK(rig.cap.lastBody == rig.layer.buffer().data());    // body IS the dense box buffer (no copy)
}

// Sparse layout: the buffered send streams the LUT-mapped DRIVER buffer (only the real lights, in
// driver order), exactly like the LED drivers — NOT the dense bounding box. So coordCount == the
// shell count and the frame is sent whole at full res through the resumable path.
TEST_CASE("PreviewDriver buffered send uses the sparse driver buffer, not the dense box") {
    mm::SphereLayout s; s.radius = 4;            // 210 shell lights in a 9^3 = 729 box
    PreviewRig rig(&s);
    rig.cap.bufferedFrames = 0;
    rig.produce();
    CHECK(rig.cap.bufferedFrames == 1);                      // full res → resumable buffered send
    CHECK(rig.cap.frameCount() == 210);                      // the SHELL, not 729 — the driver buffer
    CHECK(rig.cap.frameCount() == rig.cap.coordCount());     // table + frame agree (lockstep)
    CHECK(rig.cap.lastBody != rig.layer.buffer().data());    // NOT the dense box — the mapped output
}

// Dense-grid CLOSED-FORM downsample, exact colour placement: a 200×1 strip pinned over a small cap
// strides in x only, so the kept lights are columns 0,s,2s,… The colour pass must read each from its
// dense buffer index (closed-form x for a 1-row grid) and pack them in the SAME order as the coord
// table — no forEachCoord. Painting a known colour at a kept column and finding it at the matching
// frame position pins the index math + the lattice order.
TEST_CASE("PreviewDriver dense downsample packs colours by closed-form index, in lattice order") {
    const int width = 5000;                            // > the 4096 display cap → forces a stride
    mm::GridLayout g; g.width = width; g.height = 1; g.depth = 1;
    PreviewRig rig(&g);
    const int s = rig.cap.coordStride();
    REQUIRE(s >= 2);                                   // 5000 cols over the 4096 cap → strided in x
    const int kept = rig.cap.coordCount();
    REQUIRE(kept == (width + s - 1) / s);              // ceil(width/s) — closed-form count

    // Paint the 2nd kept column (x = s) bright green; the rest black.
    uint8_t* buf = rig.layer.buffer().data();
    std::memset(buf, 0, static_cast<size_t>(width) * 3);
    buf[s * 3 + 1] = 150;                              // G at column x=s (the 2nd kept light)

    rig.preview->sendFrame();
    // 0x02 = 7-byte hdr + (r,g,b)×kept. The 2nd kept light is the 2nd triple → its G byte at 7+3+1.
    REQUIRE(rig.cap.lastFrame.size() == 7u + static_cast<size_t>(kept) * 3u);
    CHECK(rig.cap.lastFrame[7 + 3 + 1] == 150);        // painted column landed at the 2nd position
    CHECK(rig.cap.lastFrame[7 + 1] == 0);              // column 0 is black (1st position)

    mm::platform::setTestMaxAllocBlock(0);
}

// ADAPTIVE FRAME RATE: while a buffered send is still draining (a slow link), loop() must NOT start
// a new frame — it waits for bufferedSendIdle(). So the effective rate self-limits to the link.
TEST_CASE("PreviewDriver gates the next frame on the buffered send draining (adaptive fps)") {
    mm::GridLayout g; g.width = 16; g.height = 16; g.depth = 1;
    PreviewRig rig(&g);
    rig.cap.bufferedDrains = 3;        // each send stays "in flight" for 3 idle-polls (slow link)

    uint32_t t = 1000;
    auto tick = [&] { t += 100; mm::platform::setTestNowMs(t); rig.preview->loop(); };

    tick();                            // first loop: coord table + first buffered frame starts
    const int after1 = rig.cap.bufferedFrames;
    CHECK(after1 == 1);
    tick();                            // send still draining → must NOT start a second frame
    tick();
    CHECK(rig.cap.bufferedFrames == after1);   // gated: no new frame while busy
    CHECK(rig.cap.bufferedDropped == 0);       // it WAITED, didn't spam-and-drop

    mm::platform::setTestNowMs(0);
}

// USE-AFTER-FREE GUARD: a geometry rebuild (resize) frees+reallocs the producer buffer, so any
// in-flight buffered send (which holds a pointer into it) MUST be cancelled in onBuildState before
// the buffer goes away — else drainPreviewSend would read freed memory.
TEST_CASE("PreviewDriver cancels an in-flight buffered send on rebuild (resize safety)") {
    mm::GridLayout g; g.width = 16; g.height = 16; g.depth = 1;
    PreviewRig rig(&g);
    rig.cap.bufferedDrains = 10;       // keep a send "in flight" so a rebuild interrupts it

    rig.preview->sendFrame();          // start a buffered send (stays active for 10 polls)
    CHECK(rig.cap.bufferedFrames >= 1);
    const int cancelsBefore = rig.cap.bufferedCanceled;

    // A resize: rebuild the pipeline. PreviewDriver::onBuildState must cancel the active send
    // BEFORE the buffer is reallocated.
    g.width = 32; g.height = 32;
    rig.layer.onBuildState();          // reallocs the producer buffer (the body the send pointed at)
    rig.preview->onBuildState();       // must cancel the in-flight send

    CHECK(rig.cap.bufferedCanceled == cancelsBefore + 1);   // the stale send was cancelled
}
