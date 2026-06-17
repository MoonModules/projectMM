#pragma once

#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/effects/EffectBase.h"
#include "light/layers/MappingLUT.h"
#include "light/modifiers/ModifierBase.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>  // std::memcpy in extrude()

namespace mm {

class Layer : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Layer; }
    const char* acceptsChildRoles() const override { return "effect,modifier"; }

    // start/end carve a region of the shared Layouts into this Layer's buffer,
    // expressed as **percentages of the physical extent on each axis**.
    // `start = 0, end = 100` is the full layout — the defaults below — and is
    // byte-identical to the pre-Layers pipeline. Percentages are resilient to
    // physical layout changes: a `startX = 25` Layer stays at the same
    // relative position when the panel resizes from 64×64 to 128×128, rather
    // than ending up at the wrong absolute pixel.
    //
    // Negative values and values > 100 are legal: a future modifier could drag
    // a Layer in or out of the visible area by shifting start/end past 0% or
    // 100% (e.g. `startX = -50` means the Layer extends 50% off the left edge
    // of the layout). ControlType::Int16 is the wire type so negative values
    // round-trip correctly through /api/state, /api/types, and persistence.
    // `lengthType` (int16_t) is reused so the type matches width/height/depth
    // — the *semantics* (percent vs pixel) live in the field name and spec.
    //
    // Spec: docs/architecture.md § Layers and Layer.
    // NOTE: start/end are not yet read in onBuildState/rebuildLUT — they don't
    // affect the buffer size today, so Layer doesn't override controlChangeTriggersBuildState.
    // When they become functional (carving a sub-region → different buffer size), add
    // `bool controlChangeTriggersBuildState(const char*) const override { return true; }` so a
    // start/end change triggers the pipeline rebuild.
    lengthType startX = 0;
    lengthType startY = 0;
    lengthType startZ = 0;
    lengthType endX = 100;
    lengthType endY = 100;
    lengthType endZ = 100;

    void onBuildControls() override {
        // Names match the field names; the percent semantic lives in the spec
        // (Layer.md § start/end controls) and is reflected in the comment above.
        controls_.addInt16("startX", startX);
        controls_.addInt16("startY", startY);
        controls_.addInt16("startZ", startZ);
        controls_.addInt16("endX",   endX);
        controls_.addInt16("endY",   endY);
        controls_.addInt16("endZ",   endZ);
        // Cascade to children (effects and modifiers) — preserves the default
        // base behaviour we just overrode.
        MoonModule::onBuildControls();
    }

    void setLayouts(Layouts* lg) { layouts_ = lg; }
    // The active Layouts, for consumers that need per-light coordinates (e.g.
    // PreviewDriver builds its coordinate table from layouts()->forEachCoord).
    Layouts* layouts() const { return layouts_; }
    void setChannelsPerLight(uint8_t cpl) { channelsPerLight_ = cpl; }

    void onBuildState() override {
        // Treat "no layouts wired" the same as "every layout child disabled" —
        // either way the Layer should be empty (no LUT, no buffer, zero dims).
        // Returning early here used to leave stale state from a previous build,
        // which Drivers then read as a sized LUT pointing at a null buffer.
        const nrOfLightsType physicalCount = layouts_ ? layouts_->totalLightCount() : 0;

        // Empty layout (every layout child disabled, or no layouts wired): tear
        // down the LUT and buffer and report zero dims. Bailing out without
        // dropping the old state left the LUT sized for the previous layout
        // while Drivers reallocated its output buffer to 0 bytes (a stale LUT
        // + null output buffer = blendMap dereferences null on the next tick).
        // After this branch hasLUT() is false and physicalLightCount() is 0,
        // so Drivers::onBuildState takes the "no LUT" path and Drivers::loop
        // skips blendMap entirely.
        if (physicalCount == 0) {
            physicalWidth_ = physicalHeight_ = physicalDepth_ = 0;
            width_ = height_ = depth_ = 0;
            lut_.free();
            buffer_.free();
            setDynamicBytes(0);
            // Clear stale degrade state from a previous build — both the status
            // string AND lutSkipped_. Without resetting the flag, lutSkipped()
            // keeps reporting true even though we just freed the LUT.
            lutSkipped_ = false;
            clearStatus();
            MoonModule::onBuildState();
            return;
        }

        // Compute physical dimensions from layout
        struct DimCtx { lengthType maxX, maxY, maxZ; };
        DimCtx dctx{0, 0, 0};
        layouts_->forEachCoord([](void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
            auto* d = static_cast<DimCtx*>(ctx);
            if (x > d->maxX) d->maxX = x;
            if (y > d->maxY) d->maxY = y;
            if (z > d->maxZ) d->maxZ = z;
        }, &dctx);
        physicalWidth_ = dctx.maxX + 1;
        physicalHeight_ = dctx.maxY + 1;
        physicalDepth_ = dctx.maxZ + 1;

        rebuildLUT();

        // Neutral status: the LOGICAL box the effects render into (width_×height_×
        // depth_) — this is what start/end region carving and modifiers reshape,
        // so it can differ from the physical box (shown on Layouts). Only set it
        // when rebuildLUT left the status clear; a degrade path (LUT skipped /
        // buffer reduced) sets its own Warning, which must win over this line.
        if (status() == nullptr) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%u×%u×%u",
                          static_cast<unsigned>(width_),
                          static_cast<unsigned>(height_),
                          static_cast<unsigned>(depth_));
            setStatus(statusBuf_);
        }

        // Children allocate after LUT is built (effects need buffer dimensions)
        MoonModule::onBuildState();
    }

    void loop() override {
        // Scheduler already gates the Layer itself by enabled() via respectsEnabled().
        // We still gate per-effect-child explicitly because Layer iterates its own
        // children rather than going through the Scheduler.
        elapsed_ = platform::millis();
        buffer_.clear();
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Effect) continue;
            if (!child(i)->enabled()) continue;
            auto* eff = static_cast<EffectBase*>(child(i));
            uint32_t start = platform::micros();
            eff->loop();
            // Extrude a lower-dimensional effect across the unused axes so a D1
            // or D2 effect "just works" on a higher-dimensional grid. The effect
            // only writes its own slice (D1 → row y=0,z=0; D2 → slice z=0); the
            // framework duplicates that across the rest of the buffer.
            extrude(eff->dimensions());
            eff->addAccumUs(platform::micros() - start);
        }
        // Tick dynamic modifiers AFTER the effect pass, so the frame's buffer is fully
        // written before a modifier acts. A static modifier's loop() is empty (it only
        // shapes the LUT, rebuilt on control change). A dynamic one (RandomMapModifier)
        // may call our onBuildState() from here to rebuild the LUT on its timer — safe
        // re-entrancy precisely because every effect write for this frame is already done.
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Modifier) continue;
            if (!child(i)->enabled()) continue;
            static_cast<ModifierBase*>(child(i))->loop();
        }
    }

    // Copy the effect's written slice to fill the unused axes. Called after each
    // effect's loop(). Buffer layout is (z * h + y) * w + x channels per light.
    //
    // Hot-path shape: D3 effects (the default) take the early return and pay
    // nothing beyond one comparison and a branch. On a 2D layout (depth=1) the
    // z-fill is naturally a no-op regardless of effectDim — the `depth_ > 1`
    // guard short-circuits. Same for D1 on a 1D layout. Real work only happens
    // when the effect declared fewer axes than the layout has.
    void extrude(Dim effectDim) {
        if (effectDim == Dim::D3) return;
        uint8_t* buf = buffer_.data();
        if (!buf) return;
        const size_t cpl = channelsPerLight_;
        const size_t rowBytes = static_cast<size_t>(width_) * cpl;
        const size_t sliceBytes = rowBytes * height_;

        // D1: the effect wrote row (y=0, z=0). Duplicate it across all y in z=0.
        if (effectDim == Dim::D1 && height_ > 1) {
            for (lengthType y = 1; y < height_; y++) {
                std::memcpy(buf + y * rowBytes, buf, rowBytes);
            }
        }
        // D1 and D2: at this point z=0 holds a complete (possibly extruded) slice.
        // Duplicate it across all z > 0.
        if (depth_ > 1) {
            for (lengthType z = 1; z < depth_; z++) {
                std::memcpy(buf + z * sliceBytes, buf, sliceBytes);
            }
        }
    }

    Buffer& buffer() { return buffer_; }
    const Buffer& buffer() const { return buffer_; }
    const MappingLUT& lut() const { return lut_; }

    // Effects see logical dimensions
    lengthType width() const { return width_; }
    lengthType height() const { return height_; }
    lengthType depth() const { return depth_; }
    uint8_t channelsPerLight() const { return channelsPerLight_; }
    uint32_t elapsed() const { return elapsed_; }

    nrOfLightsType physicalLightCount() const {
        return layouts_ ? layouts_->totalLightCount() : 0;
    }

    // Physical dimensions match the actual LED arrangement (computed in onBuildState from
    // the Layouts). PreviewDriver and any future driver that needs to describe the LED
    // shape should read these rather than caching values from main.cpp startup.
    lengthType physicalWidth() const { return physicalWidth_; }
    lengthType physicalHeight() const { return physicalHeight_; }
    lengthType physicalDepth() const { return physicalDepth_; }

    bool lutSkipped() const { return lutSkipped_; }

    // Precondition: physicalWidth_/Height_/Depth_ must be set (call from onBuildState)
    void rebuildLUT() {
        lutSkipped_ = false;
        clearStatus();  // re-evaluated below if a degrade path is taken

        // Find first enabled modifier (if any)
        ModifierBase* mod = nullptr;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() == ModuleRole::Modifier && child(i)->enabled()) {
                mod = static_cast<ModifierBase*>(child(i));
                break;
            }
        }

        // Box vs real-light count. The render grid is the layout's bounding box
        // (physicalWidth_×Height_×Depth_); the DRIVER buffer holds only the real
        // lights (Layouts::totalLightCount). For a dense GridLayout every box
        // cell is a light → boxCount == driverCount and the LUT is identity (the
        // fast memcpy path). For a sparse layout (sphere, ring) driverCount <
        // boxCount: most box cells map to NO light, so we always build a LUT
        // whose destinations are DRIVER indices [0..driverCount). This makes the
        // driver/output buffer — what ArtNet, LEDs, and the preview consume —
        // exactly the real lights, never the padded box.
        nrOfLightsType boxCount = static_cast<nrOfLightsType>(physicalWidth_) * physicalHeight_ * physicalDepth_;
        nrOfLightsType driverCount = physicalLightCount();   // == Layouts::totalLightCount()
        const bool sparse = (driverCount != boxCount);

        if (!mod) {
            // No modifier: logical == physical box. Effects render into the dense
            // box buffer; the LUT extracts the real lights into the driver buffer.
            width_ = physicalWidth_;
            height_ = physicalHeight_;
            depth_ = physicalDepth_;
            if (!sparse) {
                // Dense grid: box cell i IS light i. Identity (memcpy) — unchanged.
                lut_.setIdentity(boxCount);
                allocateBuffer(boxCount);
                return;
            }
            // Sparse: build box→driver LUT (each box cell → its driver index, or
            // nothing). logicalCount = boxCount, ≤1 destination per cell.
            buildSparseIdentityLUT(boxCount, driverCount);
            allocateBuffer(boxCount);   // layer (render) buffer stays the dense box
            return;
        }

        // Apply first static modifier to compute logical dimensions
        mod->logicalDimensions(physicalWidth_, physicalHeight_, physicalDepth_,
                               width_, height_, depth_);

        nrOfLightsType logicalCount = static_cast<nrOfLightsType>(width_) * height_ * depth_;

        // Degrade target on OOM: a sparse layout degrades to the (cheaper)
        // box→driver identity LUT so the driver buffer stays the real lights; a
        // dense grid degrades to the plain identity/memcpy path as before.
        auto degradeIdentity = [&]() {
            width_ = physicalWidth_;
            height_ = physicalHeight_;
            depth_ = physicalDepth_;
            if (sparse) { buildSparseIdentityLUT(boxCount, driverCount); }
            else        { lut_.setIdentity(boxCount); }
            allocateBuffer(boxCount);
        };

        // maxDest in DRIVER-index terms. The modifier's multiplier bounds how
        // many destinations a logical cell fans out to; clamp to 2× the real
        // light count (was box count — driverCount is the true ceiling now).
        // Compute in 64-bit: on a no-PSRAM board nrOfLightsType is uint16, and
        // logicalCount × maxMultiplier (e.g. 256 × 256) overflows it — which
        // wrapped maxDest to a tiny value, built a near-empty LUT, and blanked
        // the display (the multiplyZ-on-2D black-screen bug). Clamp the ceiling
        // before narrowing back to nrOfLightsType.
        uint64_t maxDestWide = static_cast<uint64_t>(logicalCount) * mod->maxMultiplier();
        const uint64_t ceiling = static_cast<uint64_t>(driverCount) * 2;
        if (maxDestWide > ceiling) maxDestWide = ceiling;
        nrOfLightsType maxDest = static_cast<nrOfLightsType>(maxDestWide);

        // MappingLUT::build owns the allocation decision: it tries a single
        // contiguous block, falls back to fixed-size pages when no single block
        // fits but total heap allows it, and returns false only on genuine
        // exhaustion (total free minus HEAP_RESERVE too small). So no
        // single-block pre-check here — that pre-check is exactly what made a
        // fragmented-but-not-exhausted heap (the 128×128 mirror case on
        // no-PSRAM) degrade unnecessarily.
        if (!lut_.build(logicalCount, maxDest)) {
            std::printf("  DEGRADE  LUT skipped (need %u, free %u)\n",
                        static_cast<unsigned>(MappingLUT::estimateBytes(logicalCount, maxDest)),
                        static_cast<unsigned>(platform::freeHeap()));
            lutSkipped_ = true;
            setStatus("modifier LUT skipped — not enough memory", Severity::Warning);
            degradeIdentity();
            return;
        }

        // Box→driver translation: the modifier reasons in box geometry and emits
        // box-cell indices; the driver buffer is indexed by real-light index. A
        // box→driver map (driverIdx per box cell, or "none") translates the
        // modifier's output into driver-index space. For a dense grid this map
        // is identity; for a sparse layout it drops box cells that aren't lights
        // (e.g. a mirror destination landing off the sphere shell). Built from
        // Layouts::forEachCoord. nullptr → dense grid → no translation needed.
        nrOfLightsType* boxToDriver = sparse ? buildBoxToDriver(boxCount) : nullptr;

        // Per-light scratch buffer for the modifier's fan-out destinations. Sized
        // to the modifier's maxMultiplier(), but clamped to boxCount: a logical
        // light can't map to more positions than the physical box has cells, so
        // that's the true ceiling. The clamp avoids a pathological over-alloc
        // when maxMultiplier() saturates high (e.g. a 64×64×16 multiply reports
        // 65535 but a small grid has far fewer cells). Heap-allocated on the cold
        // path (like boxToDriver); an OOM here degrades to the identity LUT.
        nrOfLightsType fanout = mod->maxMultiplier() > 0 ? mod->maxMultiplier() : 1;
        if (fanout > boxCount && boxCount > 0) fanout = boxCount;
        auto* physicals = static_cast<nrOfLightsType*>(
            platform::alloc(static_cast<size_t>(fanout) * sizeof(nrOfLightsType)));
        if (!physicals) {
            if (boxToDriver) platform::free(boxToDriver);
            lut_.free();
            lutSkipped_ = true;
            setStatus("modifier scratch alloc failed — not enough memory", Severity::Warning);
            degradeIdentity();
            return;
        }
        nrOfLightsType count;
        nrOfLightsType logIdx = 0;

        for (lengthType z = 0; z < depth_; z++) {
            for (lengthType y = 0; y < height_; y++) {
                for (lengthType x = 0; x < width_; x++) {
                    count = 0;
                    mod->mapToPhysical(x, y, z, physicalWidth_, physicalHeight_, physicalDepth_,
                                       physicals, count, fanout);
                    if (boxToDriver) {
                        // Translate box indices → driver indices, dropping cells
                        // that map to no real light (kNoDriver).
                        nrOfLightsType kept = 0;
                        for (nrOfLightsType i = 0; i < count; i++) {
                            nrOfLightsType d = (physicals[i] < boxCount) ? boxToDriver[physicals[i]] : kNoDriver;
                            if (d != kNoDriver) physicals[kept++] = d;
                        }
                        count = kept;
                    }
                    lut_.setMapping(logIdx, physicals, count);
                    logIdx++;
                }
            }
        }
        platform::free(physicals);
        if (boxToDriver) platform::free(boxToDriver);

        lut_.finalize();
        allocateBuffer(logicalCount);
    }

    // Sentinel: a box cell that is not a real light (no driver index).
    static constexpr nrOfLightsType kNoDriver = static_cast<nrOfLightsType>(-1);

    // Allocate + fill a box-cell → driver-index map from the layout's real
    // lights (Layouts::forEachCoord emits (driverIdx, x, y, z) in driver order).
    // Cells with no light hold kNoDriver. Caller owns the returned block
    // (platform::free). Returns nullptr on OOM. Box index = z*W*H + y*W + x.
    nrOfLightsType* buildBoxToDriver(nrOfLightsType boxCount) const {
        auto* map = static_cast<nrOfLightsType*>(platform::alloc(static_cast<size_t>(boxCount) * sizeof(nrOfLightsType)));
        if (!map) return nullptr;
        for (nrOfLightsType i = 0; i < boxCount; i++) map[i] = kNoDriver;
        struct Ctx { nrOfLightsType* map; lengthType w, h; nrOfLightsType box; };
        Ctx ctx{map, physicalWidth_, physicalHeight_, boxCount};
        layouts_->forEachCoord([](void* c, nrOfLightsType driverIdx, lengthType x, lengthType y, lengthType z) {
            auto* k = static_cast<Ctx*>(c);
            nrOfLightsType box = static_cast<nrOfLightsType>(z) * k->w * k->h
                               + static_cast<nrOfLightsType>(y) * k->w + x;
            if (box < k->box) k->map[box] = driverIdx;
        }, &ctx);
        return map;
    }

    // No-modifier sparse case: LUT maps each box cell → its driver index (≤1
    // destination), so the driver buffer holds only the real lights. logicalCount
    // = boxCount; built in box order so setMapping's sequential contract holds.
    void buildSparseIdentityLUT(nrOfLightsType boxCount, nrOfLightsType driverCount) {
        nrOfLightsType* boxToDriver = buildBoxToDriver(boxCount);
        if (!boxToDriver || !lut_.build(boxCount, driverCount)) {
            // OOM — fall back to dense identity (sends the box; correct-not-crash).
            if (boxToDriver) platform::free(boxToDriver);
            lutSkipped_ = true;
            setStatus("sparse LUT build failed — not enough memory", Severity::Warning);
            lut_.setIdentity(boxCount);
            return;
        }
        for (nrOfLightsType box = 0; box < boxCount; box++) {
            nrOfLightsType d = boxToDriver[box];
            lut_.setMapping(box, &d, d == kNoDriver ? 0 : 1);
        }
        lut_.finalize();
        platform::free(boxToDriver);
    }

private:
    Layouts* layouts_ = nullptr;
    Buffer buffer_;
    MappingLUT lut_;
    uint8_t channelsPerLight_ = 3;
    bool lutSkipped_ = false;
    lengthType physicalWidth_ = 0;
    lengthType physicalHeight_ = 0;
    lengthType physicalDepth_ = 0;
    lengthType width_ = 0;  // logical (what effects see)
    lengthType height_ = 0;
    lengthType depth_ = 0;
    uint32_t elapsed_ = 0;
    char statusBuf_[20] = {};  // "999×999×999" fits; owned (setStatus borrows the pointer)

    // Check if heap can afford an allocation (returns true if unlimited or enough budget)
    static bool canAllocate(size_t bytesNeeded) {
        size_t availableHeap = platform::freeHeap();
        if (availableHeap == 0) return true; // desktop: unlimited
        size_t internalHeap = platform::freeInternalHeap();
        if (internalHeap > 0 && internalHeap <= platform::HEAP_RESERVE) return false;
        size_t budget = availableHeap > platform::HEAP_RESERVE ? availableHeap - platform::HEAP_RESERVE : 0;
        return budget >= bytesNeeded && platform::maxAllocBlock() >= bytesNeeded;
    }

    void allocateBuffer(nrOfLightsType count) {
        // Try to allocate buffer, halve dimensions if needed
        bool reduced = false;
        while (count > 0) {
            size_t needed = static_cast<size_t>(count) * channelsPerLight_;
            if (canAllocate(needed)) {
                if (buffer_.allocate(count, channelsPerLight_)) {
                    setDynamicBytes(buffer_.bytes() + lut_.memoryUsed());
                    if (reduced) setStatus("buffer reduced — not enough memory", Severity::Warning);
                    return;
                }
                // allocate returned false despite canAllocate check — degrade
                std::printf("  DEGRADE  buffer_.allocate failed for %u lights\n",
                            static_cast<unsigned>(count));
            }
            // Halve: reduce to sqrt of count (halve each dimension)
            width_ = width_ > 1 ? width_ / 2 : 1;
            height_ = height_ > 1 ? height_ / 2 : 1;
            depth_ = depth_ > 1 ? depth_ / 2 : 1;
            count = static_cast<nrOfLightsType>(width_) * height_ * depth_;
            reduced = true;
            std::printf("  DEGRADE  buffer too large, reducing to %dx%dx%d\n",
                        static_cast<int>(width_), static_cast<int>(height_), static_cast<int>(depth_));
            if (width_ <= 8 && height_ <= 8) break; // minimum
        }
        if (!buffer_.allocate(count, channelsPerLight_)) {
            std::printf("  DEGRADE  buffer_.allocate failed at minimum size %u\n",
                        static_cast<unsigned>(count));
            setStatus("buffer allocation failed — not enough memory", Severity::Error);
        } else if (reduced) {
            setStatus("buffer reduced — not enough memory", Severity::Warning);
        }
        setDynamicBytes(buffer_.bytes() + lut_.memoryUsed());
    }
};

// EffectBase accessor implementations
inline Layer* EffectBase::layer() const { return static_cast<Layer*>(parent()); }
inline uint8_t* EffectBase::buffer() { return layer()->buffer().data(); }
inline lengthType EffectBase::width() const { return layer()->width(); }
inline lengthType EffectBase::height() const { return layer()->height(); }
inline lengthType EffectBase::depth() const { return layer()->depth(); }
inline uint8_t EffectBase::channelsPerLight() const { return layer()->channelsPerLight(); }
inline nrOfLightsType EffectBase::nrOfLights() const { return layer()->buffer().count(); }
inline uint32_t EffectBase::elapsed() const { return layer()->elapsed(); }

} // namespace mm
