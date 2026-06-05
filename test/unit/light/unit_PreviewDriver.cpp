// @module PreviewDriver

#include "doctest.h"
#include "light/drivers/PreviewDriver.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"

// PreviewDriver downsamples the render buffer into a small RGB frame so the
// whole WebSocket message fits lwIP's TCP send buffer. These tests verify that
// the `detail` control selects the right voxel budget (and therefore stride),
// that the frame always carries the original grid dimensions (needed by the
// `decompress` UI hint), and that even the finest setting stays within the
// send-buffer payload limit.

namespace {

// Records each broadcastBinary call so tests can assert a frame was produced
// AND pushed (replacing the old PreviewFrame::ready poll flag). Captures the
// last payload's total length too, to cross-check header + data size.
struct RecordingBroadcaster : mm::BinaryBroadcaster {
    int calls = 0;
    size_t lastTotalLen = 0;
    void broadcastBinary(const mm::platform::WriteChunk* payload, int chunkCount) override {
        calls++;
        lastTotalLen = 0;
        for (int i = 0; i < chunkCount; i++) lastTotalLen += payload[i].len;
    }
};

// Build a populated PreviewDriver against a GridLayout of the given size.
// Returns by wiring the caller's objects — keeps every object on the stack so
// there is no allocation outside PreviewDriver's own owned buffer.
struct PreviewRig {
    mm::GridLayout grid;
    mm::Layouts group;
    mm::Layer layer;
    mm::Buffer source;
    mm::PreviewFrame frame;
    RecordingBroadcaster broadcaster;
    mm::PreviewDriver driver;

    PreviewRig(mm::lengthType w, mm::lengthType h, mm::lengthType d, uint8_t cpl) {
        grid.width = w;
        grid.height = h;
        grid.depth = d;
        group.addChild(&grid);
        layer.setLayouts(&group);
        layer.setChannelsPerLight(cpl);
        layer.onBuildState();

        source.allocate(static_cast<mm::nrOfLightsType>(w) * h * d, cpl);
        // Fill so the strided copy produces non-zero output.
        for (size_t i = 0; i < source.bytes(); i++) source.data()[i] = 0x40;

        driver.setLayer(&layer);
        driver.setSourceBuffer(&source);
        driver.setPreviewFrame(&frame);
        driver.setBroadcaster(&broadcaster);
        driver.onBuildControls();
        driver.onBuildState();
    }

    // Produce one frame deterministically. renderFrame() bypasses loop()'s fps
    // rate-limit, so the test never sleeps and never depends on wall-clock time.
    // renderFrame() also pushes to the broadcaster, so broadcaster.calls rises.
    void produceFrame() {
        driver.renderFrame();
    }
};

} // namespace

// The three `detail` levels (1/2/3) downsample a 128-axis grid into 16/32/43 axes — distinct strides so the levels are visibly different.
TEST_CASE("PreviewDriver detail levels select distinct strides on a 128 grid") {
    // 128-axis grid: detail 1/2/3 budgets (256/1024/1849) must land on
    // distinct strides so the three settings are visibly different.
    struct Case { uint8_t detail; mm::lengthType expectDim; };
    const Case cases[] = {
        {1, 16}, // stride 8 → ceil(128/8) = 16  (256 voxels)
        {2, 32}, // stride 4 → ceil(128/4) = 32  (1024 voxels)
        {3, 43}, // stride 3 → ceil(128/3) = 43  (1849 voxels)
    };

    for (const auto& c : cases) {
        PreviewRig rig(128, 128, 1, 3);
        rig.driver.detail = c.detail;
        rig.produceFrame();

        REQUIRE(rig.broadcaster.calls > 0);  // frame produced AND pushed to the WS broadcaster
        CHECK(rig.frame.width == c.expectDim);
        CHECK(rig.frame.height == c.expectDim);
        CHECK(rig.frame.depth == 1);
        // Payload = downsampled voxels × 3 RGB bytes.
        CHECK(rig.frame.dataLen ==
              static_cast<size_t>(c.expectDim) * c.expectDim * 1 * 3);
    }
}

// Even when downsampled, the frame carries the original grid dimensions so the UI's `decompress` hint can block-replicate back.
TEST_CASE("PreviewDriver frame carries the original grid dimensions") {
    // The `decompress` UI hint block-replicates back to the real grid, so the
    // frame must always report the original (pre-downsample) dimensions.
    PreviewRig rig(128, 64, 1, 3);
    rig.driver.detail = 1; // coarse → downsampled dims differ from original
    rig.produceFrame();

    REQUIRE(rig.broadcaster.calls > 0);  // frame produced AND pushed to the WS broadcaster
    CHECK(rig.frame.origWidth == 128);
    CHECK(rig.frame.origHeight == 64);
    CHECK(rig.frame.origDepth == 1);
    // Downsampled dims are smaller than the original.
    CHECK(rig.frame.width < rig.frame.origWidth);
    CHECK(rig.frame.height < rig.frame.origHeight);
}

// detail=3 (largest payload) stays under lwIP's ~5.7 KB TCP send buffer so writeChunks completes in one whole pass.
TEST_CASE("PreviewDriver finest detail stays within the send-buffer budget") {
    // detail = 3 is the largest payload. 13-byte header + RGB must fit lwIP's
    // ~5.7 KB TCP send buffer so the non-blocking writeChunks completes whole.
    constexpr size_t MAX_VOXELS = 1849;
    constexpr size_t HEADER = 13;
    constexpr size_t SEND_BUFFER_LIMIT = 5760;

    PreviewRig rig(128, 128, 1, 3);
    rig.driver.detail = 3;
    rig.produceFrame();

    REQUIRE(rig.broadcaster.calls > 0);  // frame produced AND pushed to the WS broadcaster
    size_t voxels = static_cast<size_t>(rig.frame.width) *
                    rig.frame.height * rig.frame.depth;
    CHECK(voxels <= MAX_VOXELS);
    CHECK(rig.frame.dataLen + HEADER <= SEND_BUFFER_LIMIT);
}

// A small grid (≤ budget) is copied 1:1 with no downsampling — preview matches the original exactly.
TEST_CASE("PreviewDriver small grid is copied 1:1 (stride 1)") {
    // A grid well under the budget needs no downsampling — every voxel copied,
    // so a less detailed preview means a higher driver FPS (fewer bytes).
    PreviewRig rig(8, 8, 1, 3);
    rig.driver.detail = 2;
    rig.produceFrame();

    REQUIRE(rig.broadcaster.calls > 0);  // frame produced AND pushed to the WS broadcaster
    CHECK(rig.frame.width == 8);
    CHECK(rig.frame.height == 8);
    CHECK(rig.frame.dataLen == 8 * 8 * 3);
}

// On RGBW (4-channel) sources the preview keeps only the first 3 channels — the wire frame is always 3 bytes per voxel.
TEST_CASE("PreviewDriver channel-agnostic: RGBW source copies RGB only") {
    // The strided copy takes the first 3 channels regardless of channelsPerLight,
    // so RGBW / multi-channel layouts preview correctly (3 B per voxel on wire).
    PreviewRig rig(16, 16, 1, 4); // RGBW
    rig.driver.detail = 3;        // small grid → stride 1, full copy
    rig.produceFrame();

    REQUIRE(rig.broadcaster.calls > 0);  // frame produced AND pushed to the WS broadcaster
    CHECK(rig.frame.width == 16);
    CHECK(rig.frame.height == 16);
    CHECK(rig.frame.dataLen == 16 * 16 * 3); // 3 B/voxel even with 4-ch source
}

// Default controls: fps=24 (preview stream rate), detail=3 (finest), decompress=true.
TEST_CASE("PreviewDriver fps default is the rate-limited preview rate") {
    // fps default is the preview *stream* rate (independent of render FPS).
    mm::PreviewDriver driver;
    CHECK(driver.fps == 24);
    CHECK(driver.detail == 3);
    CHECK(driver.decompress == true);
}
