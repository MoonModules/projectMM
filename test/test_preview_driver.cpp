#include "doctest.h"
#include "light/drivers/PreviewDriver.h"
#include "light/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "core/PreviewFrame.h"

// PreviewDriver downsamples the render buffer into a small RGB frame so the
// whole WebSocket message fits lwIP's TCP send buffer. These tests verify that
// the `detail` control selects the right voxel budget (and therefore stride),
// that the frame always carries the original grid dimensions (needed by the
// `decompress` UI hint), and that even the finest setting stays within the
// send-buffer payload limit.

namespace {

// Build a populated PreviewDriver against a GridLayout of the given size.
// Returns by wiring the caller's objects — keeps every object on the stack so
// there is no allocation outside PreviewDriver's own owned buffer.
struct PreviewRig {
    mm::GridLayout grid;
    mm::Layouts group;
    mm::Layer layer;
    mm::Buffer source;
    mm::PreviewFrame frame;
    mm::PreviewDriver driver;

    PreviewRig(mm::lengthType w, mm::lengthType h, mm::lengthType d, uint8_t cpl) {
        grid.width = w;
        grid.height = h;
        grid.depth = d;
        group.addChild(&grid);
        layer.setLayouts(&group);
        layer.setChannelsPerLight(cpl);
        layer.onAllocateMemory();

        source.allocate(static_cast<mm::nrOfLightsType>(w) * h * d, cpl);
        // Fill so the strided copy produces non-zero output.
        for (size_t i = 0; i < source.bytes(); i++) source.data()[i] = 0x40;

        driver.setLayer(&layer);
        driver.setSourceBuffer(&source);
        driver.setPreviewFrame(&frame);
        driver.onBuildControls();
        driver.onAllocateMemory();
    }

    // Produce one frame deterministically. renderFrame() bypasses loop()'s fps
    // rate-limit, so the test never sleeps and never depends on wall-clock time.
    void produceFrame() {
        driver.renderFrame();
    }
};

} // namespace

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

        REQUIRE(rig.frame.ready);
        CHECK(rig.frame.width == c.expectDim);
        CHECK(rig.frame.height == c.expectDim);
        CHECK(rig.frame.depth == 1);
        // Payload = downsampled voxels × 3 RGB bytes.
        CHECK(rig.frame.dataLen ==
              static_cast<size_t>(c.expectDim) * c.expectDim * 1 * 3);
    }
}

TEST_CASE("PreviewDriver frame carries the original grid dimensions") {
    // The `decompress` UI hint block-replicates back to the real grid, so the
    // frame must always report the original (pre-downsample) dimensions.
    PreviewRig rig(128, 64, 1, 3);
    rig.driver.detail = 1; // coarse → downsampled dims differ from original
    rig.produceFrame();

    REQUIRE(rig.frame.ready);
    CHECK(rig.frame.origWidth == 128);
    CHECK(rig.frame.origHeight == 64);
    CHECK(rig.frame.origDepth == 1);
    // Downsampled dims are smaller than the original.
    CHECK(rig.frame.width < rig.frame.origWidth);
    CHECK(rig.frame.height < rig.frame.origHeight);
}

TEST_CASE("PreviewDriver finest detail stays within the send-buffer budget") {
    // detail = 3 is the largest payload. 13-byte header + RGB must fit lwIP's
    // ~5.7 KB TCP send buffer so the non-blocking writeChunks completes whole.
    constexpr size_t MAX_VOXELS = 1849;
    constexpr size_t HEADER = 13;
    constexpr size_t SEND_BUFFER_LIMIT = 5760;

    PreviewRig rig(128, 128, 1, 3);
    rig.driver.detail = 3;
    rig.produceFrame();

    REQUIRE(rig.frame.ready);
    size_t voxels = static_cast<size_t>(rig.frame.width) *
                    rig.frame.height * rig.frame.depth;
    CHECK(voxels <= MAX_VOXELS);
    CHECK(rig.frame.dataLen + HEADER <= SEND_BUFFER_LIMIT);
}

TEST_CASE("PreviewDriver small grid is copied 1:1 (stride 1)") {
    // A grid well under the budget needs no downsampling — every voxel copied,
    // so a less detailed preview means a higher driver FPS (fewer bytes).
    PreviewRig rig(8, 8, 1, 3);
    rig.driver.detail = 2;
    rig.produceFrame();

    REQUIRE(rig.frame.ready);
    CHECK(rig.frame.width == 8);
    CHECK(rig.frame.height == 8);
    CHECK(rig.frame.dataLen == 8 * 8 * 3);
}

TEST_CASE("PreviewDriver channel-agnostic: RGBW source copies RGB only") {
    // The strided copy takes the first 3 channels regardless of channelsPerLight,
    // so RGBW / multi-channel layouts preview correctly (3 B per voxel on wire).
    PreviewRig rig(16, 16, 1, 4); // RGBW
    rig.driver.detail = 3;        // small grid → stride 1, full copy
    rig.produceFrame();

    REQUIRE(rig.frame.ready);
    CHECK(rig.frame.width == 16);
    CHECK(rig.frame.height == 16);
    CHECK(rig.frame.dataLen == 16 * 16 * 3); // 3 B/voxel even with 4-ch source
}

TEST_CASE("PreviewDriver fps default is the rate-limited preview rate") {
    // fps default is the preview *stream* rate (independent of render FPS).
    mm::PreviewDriver driver;
    CHECK(driver.fps == 12);
    CHECK(driver.detail == 2);
    CHECK(driver.decompress == false);
}
