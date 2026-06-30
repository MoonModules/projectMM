#pragma once

#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/effects/EffectBase.h"
#include "light/layers/MappingLUT.h"
#include "light/layers/BlendMap.h"   // BlendOp, for blendOp()
#include "light/modifiers/ModifierBase.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>  // std::memcpy in extrude()

namespace mm {

class Layer : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Layer; }
    const char* acceptsChildRoles() const override { return "effect,modifier"; }

    ~Layer() override { if (liveScratch_) platform::free(liveScratch_); }


    // Composition parameters — INERT on the Layer (it never reads them; a Layer
    // can't know its position in the stack or what's beneath it). The Drivers
    // container reads each enabled Layer's blendMode + opacity and composites the
    // layers in container order into the physical buffer (see Drivers::loop). The
    // value lives here so it travels with the Layer through add/delete/reorder —
    // no separate, sync-prone blend list on Drivers. The bottom (first-composited)
    // layer's blendMode is moot: it fills the cleared buffer regardless.
    uint8_t blendMode = 0;     // index into kBlendModeOptions; 0 = alpha (over)
    uint8_t opacity = 255;     // 0 = invisible, 255 = full

    void onBuildControls() override {
        static constexpr const char* kBlendModeOptions[] = {"alpha", "additive"};
        controls_.addSelect("blendMode", blendMode, kBlendModeOptions, 2);
        controls_.addUint8("opacity", opacity, 0, 255);
        // Cascade to children (effects and modifiers) — preserves the default
        // base behaviour we just overrode.
        MoonModule::onBuildControls();
    }

    // How this Layer composites when stacked above another (read by Drivers).
    // Maps the blendMode select index to the BlendMap op. Index order must match
    // kBlendModeOptions above.
    BlendOp blendOp() const {
        return blendMode == 1 ? BlendOp::Additive : BlendOp::Alpha;
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
        ensureLiveScratch();   // size the live-pass snapshot here, on the cold path

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
            // only writes its own slice (D1 → column x=0,z=0; D2 → slice z=0); the
            // framework duplicates that across the rest of the buffer.
            extrude(eff->dimensions());
            eff->addAccumUs(platform::micros() - start);
        }
        // Tick EVERY enabled modifier AFTER the effect pass (the frame's buffer is
        // fully written before any modifier acts). A static modifier's loop() is empty;
        // a beat-driven one (RandomMap) sets a rebuild flag we coalesce below; a live
        // one (Rotate) advances its angle here and remaps in the live pass that follows.
        bool rebuild = false;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Modifier || !child(i)->enabled()) continue;
            auto* m = static_cast<ModifierBase*>(child(i));
            m->loop();
            rebuild |= m->consumeNeedsRebuild();
        }
        // One rebuild per frame even if several modifiers asked (no re-entrant rebuild
        // from inside a modifier's loop()). onBuildState rebuilds the whole pipeline,
        // which re-runs rebuildLUT() with the modifiers' fresh state.
        if (rebuild) { onBuildState(); return; }

        // Live pass: remap the logical buffer per frame for dynamic modifiers (Rotate).
        // Skipped entirely when no modifier is live — a static-only chain pays nothing,
        // the buffer goes straight to the driver scatter (the pay-for-what-you-use rule).
        if (hasLive_) applyLivePass();
    }

    // COLD path (called from onBuildState after rebuildLUT): (re)size the live-pass
    // snapshot buffer to the current logical buffer, or free it when no modifier is live.
    // Keeping the alloc here means applyLivePass() on the render path only memcpys —
    // never allocates — and the scratch isn't held pinned once live modifiers are removed.
    void ensureLiveScratch() {
        const size_t bytes = hasLive_ ? buffer_.bytes() : 0;
        if (bytes == liveScratchBytes_ && (bytes != 0) == (liveScratch_ != nullptr)) return;
        if (liveScratch_) { platform::free(liveScratch_); liveScratch_ = nullptr; }
        liveScratchBytes_ = 0;
        if (bytes == 0) return;                       // no live modifier → no scratch held
        liveScratch_ = static_cast<uint8_t*>(platform::alloc(bytes));
        if (liveScratch_) liveScratchBytes_ = bytes;  // alloc-fail → applyLivePass no-ops, static frame shows
    }

    // Per-frame backward gather for live (animated) modifiers. For each DESTINATION
    // logical cell, fold its coordinate through the enabled live modifiers to the SOURCE
    // cell it samples, and copy that source pixel in — so no destination is left torn
    // (backward mapping, the textbook reason image warping samples backward). Reads from
    // a snapshot (liveScratch_) so a source already overwritten this pass isn't re-read.
    // Out-of-box sources leave the destination dark (cleared). Cold relative to the build
    // but on the hot path — runs only because hasLive_ gated it, and only the live
    // modifiers participate (static ones are already baked into lut_).
    void applyLivePass() {
        uint8_t* buf = buffer_.data();
        if (!buf || !liveScratch_) return;   // scratch is sized on the cold path (ensureLiveScratch)
        const size_t cpl = channelsPerLight_;
        const size_t bytes = static_cast<size_t>(width_) * height_ * depth_ * cpl;
        if (bytes == 0 || bytes > liveScratchBytes_) return;   // hot path NEVER allocates
        std::memcpy(liveScratch_, buf, bytes);   // snapshot the source frame

        const Coord3D logical{width_, height_, depth_};
        for (lengthType z = 0; z < depth_; z++) {
            for (lengthType y = 0; y < height_; y++) {
                for (lengthType x = 0; x < width_; x++) {
                    Coord3D src{x, y, z};
                    for (uint8_t i = 0; i < childCount(); i++) {
                        if (child(i)->role() != ModuleRole::Modifier || !child(i)->enabled()) continue;
                        auto* m = static_cast<ModifierBase*>(child(i));
                        if (m->hasModifyLive()) m->modifyLive(src, logical);
                    }
                    const size_t dstIdx = (static_cast<size_t>(z) * height_ * width_ +
                                           static_cast<size_t>(y) * width_ + x) * cpl;
                    if (src.x >= 0 && src.x < width_ && src.y >= 0 && src.y < height_ &&
                        src.z >= 0 && src.z < depth_) {
                        const size_t srcIdx = (static_cast<size_t>(src.z) * height_ * width_ +
                                               static_cast<size_t>(src.y) * width_ + src.x) * cpl;
                        std::memcpy(buf + dstIdx, liveScratch_ + srcIdx, cpl);
                    } else {
                        std::memset(buf + dstIdx, 0, cpl);   // source off-box → dark
                    }
                }
            }
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

        // 1D runs along Y: a D1 effect wrote the (x=0) column down y in z=0. Duplicate that column
        // across all x > 0, so a 1D effect expands into 2D by *adding columns to the right* — the
        // 1D output is literally the first column of its 2D form (see architecture.md §
        // Dimensionality). cpl bytes per pixel copied from the x=0 pixel of each row.
        if (effectDim == Dim::D1 && width_ > 1) {
            for (lengthType y = 0; y < height_; y++) {
                const uint8_t* src = buf + static_cast<size_t>(y) * rowBytes;   // the x=0 pixel
                for (lengthType x = 1; x < width_; x++) {
                    std::memcpy(buf + static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * cpl,
                                src, cpl);
                }
            }
        }
        // D1 and D2: z=0 now holds a complete (possibly extruded) slice — the (x,y) front face.
        // Duplicate it across all z > 0, so a 2D effect expands into 3D by adding depth slices.
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

        // Fold the box through each enabled STATIC modifier in child order — no fixed
        // chain array (Dynamic over fixed-size, architecture.md): the size pass here and
        // the per-light fold below both iterate the Layer's own (dynamic, heap-grown)
        // child list, filtering for enabled static modifiers inline, the way MoonLight's
        // `for node : nodes` does. modifyLogicalSize mutates the running box AND lets the
        // modifier stash its own output size (MoonLight's modifierSize cache), so in the
        // per-light fold each modifier reads the box at ITS OWN stage from itself.
        // A dynamic modifier (Rotate, hasModifyLive) is excluded — it remaps per frame in
        // Layer::loop's live pass, not baked into the mapping.
        uint8_t staticCount = 0;
        hasLive_ = false;
        Coord3D box{physicalWidth_, physicalHeight_, physicalDepth_};
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Modifier || !child(i)->enabled()) continue;
            auto* m = static_cast<ModifierBase*>(child(i));
            if (m->hasModifyLive()) { hasLive_ = true; continue; }   // dynamic: per-frame, not baked
            m->modifyLogicalSize(box);
            clampLogical(box);
            staticCount++;
        }

        // Final logical box = the running box after the last static modifier.
        Coord3D logical = box;
        width_ = logical.x; height_ = logical.y; depth_ = logical.z;

        const Coord3D phys{physicalWidth_, physicalHeight_, physicalDepth_};
        const nrOfLightsType boxCount    = cellCount(phys);
        const nrOfLightsType logicalCount = cellCount(logical);
        const nrOfLightsType driverCount = physicalLightCount();   // == Layouts::totalLightCount()
        const bool dense = (driverCount == boxCount);

        // Fast path — no static modifiers, dense grid in natural order: box cell i
        // IS driver light i, so the mapping is the identity memcpy. This is the FPS
        // floor for the common case; keep it before any allocation.
        if (staticCount == 0 && dense && isNaturalOrder()) {
            lut_.setIdentity(boxCount);
            allocateBuffer(boxCount);
            return;
        }

        // General build — fold each PHYSICAL light through the static chain to its
        // logical cell, accumulating the physical (driver) index onto that cell.
        // N physical lights folding onto one logical cell IS the fan-out (Multiply),
        // so each physical light contributes at most ONE destination — maxDest is
        // exactly driverCount, no product, no overflow ceiling.
        if (!buildFoldedLUT(logical, logicalCount, driverCount)) {
            // OOM in the fold build — degrade to identity (correct, not crash).
            lutSkipped_ = true;
            setStatus("modifier mapping skipped — not enough memory", Severity::Warning);
            width_ = physicalWidth_; height_ = physicalHeight_; depth_ = physicalDepth_;
            lut_.setIdentity(boxCount);
            allocateBuffer(boxCount);
            return;
        }
        allocateBuffer(logicalCount);
    }

    // Sentinel: a box cell that is not a real light (no driver index).
    static constexpr nrOfLightsType kNoDriver = static_cast<nrOfLightsType>(-1);

    // Does the layout emit lights in natural box order — driver index i == box cell i (x fastest,
    // then y, then z)? Measured, not declared: one allocation-free forEachCoord pass over the same
    // coords the build would walk, so there's a single source of truth (the coords) and no
    // per-layout hint to keep in sync. True → the dense memcpy fast path is valid; false → a
    // reordered grid (serpentine) needs the folded LUT. Only meaningful for a dense layout
    // (boxCount == driverCount); a sparse layout always routes to the folded build.
    bool isNaturalOrder() const {
        struct Ctx { lengthType w, h; bool ok; };
        Ctx ctx{physicalWidth_, physicalHeight_, true};
        layouts_->forEachCoord([](void* c, nrOfLightsType driverIdx, lengthType x, lengthType y, lengthType z) {
            auto* k = static_cast<Ctx*>(c);
            if (!k->ok) return;   // once a mismatch is found the answer is settled; skip the rest
            nrOfLightsType box = static_cast<nrOfLightsType>(z) * k->w * k->h
                               + static_cast<nrOfLightsType>(y) * k->w + x;
            if (driverIdx != box) k->ok = false;
        }, &ctx);
        return ctx.ok;
    }

    // Build the mapping by folding PHYSICAL lights to LOGICAL cells (physical→logical).
    // Our MappingLUT is a CSR keyed by logical index, and setMapping demands sequential
    // in-order writes — but folding scatters onto arbitrary, repeated logical indices.
    // So this is the textbook counting-sort CSR build: pass A counts destinations per
    // logical cell, prefix-sum to offsets, pass B scatters, then replay through
    // setMapping in logical order. Two forEachCoord passes + a counts/dests scratch,
    // all on the cold rebuild path; the hot-path read (forEachDestination) is unchanged.
    // Returns false on OOM (caller degrades to identity).
    bool buildFoldedLUT(const Coord3D& logical,
                        nrOfLightsType logicalCount, nrOfLightsType driverCount) {
        if (logicalCount == 0 || driverCount == 0) { lut_.setIdentity(0); return true; }

        // Scratch: per-logical-cell counts (then reused as the write cursor) and the
        // scattered driver indices. Each physical light yields ≤1 destination, so the
        // dests array is driverCount-sized — the tight, overflow-free ceiling.
        auto* counts = static_cast<nrOfLightsType*>(
            platform::alloc(static_cast<size_t>(logicalCount + 1) * sizeof(nrOfLightsType)));
        auto* dests = static_cast<nrOfLightsType*>(
            platform::alloc(static_cast<size_t>(driverCount) * sizeof(nrOfLightsType)));
        if (!counts || !dests) {
            if (counts) platform::free(counts);
            if (dests) platform::free(dests);
            return false;
        }
        for (nrOfLightsType i = 0; i <= logicalCount; i++) counts[i] = 0;

        // One callback does both passes. It folds the physical coord through the chain
        // (the Layer's own children — enabled static modifiers, in order, no array) to a
        // logical index (or skips it if a modifier rejects it or it lands out of box —
        // guarded, never trusted), then either counts it (pass A) or writes the driver
        // index at the cell's cursor (pass B). Everything travels through the forEachCoord
        // void* ctx, so the lambda captures nothing (it's a function ptr).
        struct FoldCtx {
            Layer* self;   // for the dynamic child list (the modifier chain)
            Coord3D logical; nrOfLightsType logicalCount;  // final box, for the flatten + guard
            nrOfLightsType* counts;   // pass A: per-cell count.  pass B: per-cell write cursor.
            nrOfLightsType* dests;    // pass B only.
            bool scatter;
        } fctx{this, logical, logicalCount, counts, dests, /*scatter=*/false};

        auto onCoord = [](void* c, nrOfLightsType driverIdx, lengthType x, lengthType y, lengthType z) {
            auto* f = static_cast<FoldCtx*>(c);
            Coord3D pos{x, y, z};
            Layer* self = f->self;
            for (uint8_t i = 0; i < self->childCount(); i++) {
                if (self->child(i)->role() != ModuleRole::Modifier || !self->child(i)->enabled()) continue;
                auto* m = static_cast<ModifierBase*>(self->child(i));
                if (m->hasModifyLive()) continue;                 // dynamic: not in the static fold
                if (!m->modifyLogical(pos)) return;               // rejected — no logical source
            }
            if (pos.x < 0 || pos.x >= f->logical.x || pos.y < 0 || pos.y >= f->logical.y ||
                pos.z < 0 || pos.z >= f->logical.z) return;                          // defensive
            const nrOfLightsType li =
                static_cast<nrOfLightsType>(pos.z) * static_cast<nrOfLightsType>(f->logical.x) * static_cast<nrOfLightsType>(f->logical.y) +
                static_cast<nrOfLightsType>(pos.y) * static_cast<nrOfLightsType>(f->logical.x) +
                static_cast<nrOfLightsType>(pos.x);
            if (li >= f->logicalCount) return;                                       // defensive
            if (f->scatter) f->dests[f->counts[li]++] = driverIdx;  // pass B: write at the cursor
            else            f->counts[li]++;                        // pass A: bump the count
        };

        // Pass A — count.
        layouts_->forEachCoord(onCoord, &fctx);

        // Prefix-sum counts → offsets (counts[li] becomes the start of cell li's run).
        nrOfLightsType running = 0;
        for (nrOfLightsType i = 0; i < logicalCount; i++) {
            nrOfLightsType c = counts[i];
            counts[i] = running;
            running += c;
        }
        counts[logicalCount] = running;   // total destinations

        // Pass B — scatter. counts[] is now the per-cell write cursor (offsets advance).
        fctx.scatter = true;
        layouts_->forEachCoord(onCoord, &fctx);

        // Pass B advanced each cell's cursor to the END of its run, so counts[i] now
        // holds the end offset of cell i — which equals the START offset of cell i+1.
        // The run for cell i is therefore [counts[i-1], counts[i]) with counts[-1]=0,
        // i.e. the `start` cursor below. dests[] is already laid out in this exact CSR
        // order, so replaying it through setMapping in logical order is a straight copy.
        if (!lut_.build(logicalCount, running)) {   // running == total destinations
            platform::free(counts);
            platform::free(dests);
            return false;
        }
        nrOfLightsType start = 0;
        for (nrOfLightsType i = 0; i < logicalCount; i++) {
            nrOfLightsType end = counts[i];          // end of cell i's run
            lut_.setMapping(i, &dests[start], static_cast<nrOfLightsType>(end - start));
            start = end;
        }
        lut_.finalize();
        platform::free(counts);
        platform::free(dests);
        return true;
    }

    // Cells in a box (the flat light count). 0 on any 0-extent axis.
    static nrOfLightsType cellCount(const Coord3D& box) {
        return static_cast<nrOfLightsType>(box.x) * static_cast<nrOfLightsType>(box.y) *
               static_cast<nrOfLightsType>(box.z);
    }

    // A modifier's modifyLogicalSize must not collapse an axis the physical box has:
    // a 0-width logical box would blank the layer with no source for any effect. Clamp
    // each axis to ≥1 where the physical box is non-empty (keep a genuinely 0 axis 0).
    void clampLogical(Coord3D& logical) const {
        if (physicalWidth_  > 0 && logical.x < 1) logical.x = 1;
        if (physicalHeight_ > 0 && logical.y < 1) logical.y = 1;
        if (physicalDepth_  > 0 && logical.z < 1) logical.z = 1;
        if (logical.x < 0) logical.x = 0;
        if (logical.y < 0) logical.y = 0;
        if (logical.z < 0) logical.z = 0;
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
    bool     hasLive_ = false;          // any enabled modifier animates per frame (gates the live pass)
    uint8_t* liveScratch_ = nullptr;    // snapshot for the live pass; allocated only when hasLive_
    size_t   liveScratchBytes_ = 0;

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
