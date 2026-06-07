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

// Captures the two preview message types so tests can inspect them.
struct CaptureBroadcaster : mm::BinaryBroadcaster {
    int coordMsgs = 0, frameMsgs = 0;
    std::vector<uint8_t> lastCoord, lastFrame;

    void broadcastBinary(const mm::platform::WriteChunk* payload, int chunkCount) override {
        std::vector<uint8_t> buf;
        for (int i = 0; i < chunkCount; i++)
            buf.insert(buf.end(), payload[i].data, payload[i].data + payload[i].len);
        if (buf.empty()) return;
        if (buf[0] == 0x03) { coordMsgs++; lastCoord = buf; }
        else if (buf[0] == 0x02) { frameMsgs++; lastFrame = buf; }
    }

    int coordCount() const { return lastCoord.size() >= 3 ? lastCoord[1] | (lastCoord[2] << 8) : -1; }
    int frameCount() const { return lastFrame.size() >= 3 ? lastFrame[1] | (lastFrame[2] << 8) : -1; }
    int coordStride() const { return lastCoord.size() >= 8 ? lastCoord[6] | (lastCoord[7] << 8) : -1; }
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
    // 0x03 = [0x03][count:u16][bx][by][bz][stride:u16] + count*3 position bytes
    CHECK(rig.cap.lastCoord.size() == 8u + 210u * 3u);
}

// Per-frame 0x02 RGB count matches the coordinate-table count.
TEST_CASE("PreviewDriver per-frame RGB count matches the coordinate table") {
    mm::SphereLayout s;
    s.radius = 4;
    PreviewRig rig(&s);
    rig.produce();

    REQUIRE(rig.cap.frameMsgs > 0);
    CHECK(rig.cap.frameCount() == 210);
    // 0x02 = [0x02][count:u16][stride:u16] + count*3 RGB bytes
    CHECK(rig.cap.lastFrame.size() == 5u + 210u * 3u);
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

// A large layout is index-downsampled (stride > 1) so the payload fits the
// send-buffer cap — but at REAL positions, not a padded box.
TEST_CASE("PreviewDriver downsamples a large layout via index stride") {
    mm::GridLayout g;
    g.width = 128; g.height = 128; g.depth = 1;   // 16384 lights > 1800-point cap
    PreviewRig rig(&g);
    rig.produce();

    CHECK(rig.cap.coordStride() > 1);             // strided
    CHECK(rig.cap.coordCount() <= 1800);          // fits the send-buffer cap
    CHECK(rig.cap.coordCount() == rig.cap.frameCount());  // table + RGB agree
}

// Default fps is the rate-limited preview stream rate.
TEST_CASE("PreviewDriver fps default") {
    mm::PreviewDriver driver;
    CHECK(driver.fps == 24);
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
