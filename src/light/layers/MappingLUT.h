#pragma once

#include "light/light_types.h"  // nrOfLightsType
#include "platform/platform.h"

#include <cstring>

namespace mm {

// CSR-style logical→physical map. `offsets_[li]..offsets_[li+1]` index a run of
// physical destinations for logical light `li`. Identity mode skips the table.
//
// The destinations array can be large for a many-to-one modifier on a big grid
// (a 128×128 XY mirror → 32768 entries × 2 B ≈ 64 KB). On a no-PSRAM ESP32 the
// largest *contiguous* free block can be smaller than that even when total free
// heap is fine — a fragmentation cliff, not exhaustion. So when a single block
// won't allocate but total heap allows it, destinations are split into
// fixed-size power-of-two PAGES that each fit a fragmented heap. Paging is the
// exception, not the rule: PSRAM boards (alloc is PSRAM-first → one huge block)
// and every no-PSRAM case where the single block fits keep the flat single
// array and the flat hot-path walk, byte-identical to a non-paged build. Only
// the one failing config (no-PSRAM + large grid + fragmented heap) pages, where
// the alternative is the modifier silently degrading to 1:1.
class MappingLUT {
public:
    MappingLUT() = default;
    ~MappingLUT() { free(); }

    MappingLUT(const MappingLUT&) = delete;
    MappingLUT& operator=(const MappingLUT&) = delete;

    // Destinations page size. Power of two so the page split/index is a
    // shift/mask (Xtensa has no hardware divide). 4096 entries × 2 B = 8 KB —
    // small enough to fit a badly fragmented heap with margin, and to stay
    // fittable as the heap shrinks with future modules.
    static constexpr nrOfLightsType kPageEntries = 4096;
    static constexpr nrOfLightsType kPageShift = 12;          // 1<<12 == 4096
    static constexpr nrOfLightsType kPageMask = kPageEntries - 1;
    static constexpr int kMaxPages = 64;                      // 64 × 4096 = 256 K dests (512 KB) cap
    static_assert((kPageEntries & kPageMask) == 0, "kPageEntries must be a power of two");

    // Fast path: logical == physical, no table needed
    void setIdentity(nrOfLightsType count) {
        free();
        identity_ = true;
        logicalCount_ = count;
    }

    // Allocate CSR arrays for 1:N mapping. Returns false only on genuine
    // exhaustion (tier 3) — the caller then degrades to 1:1. The three tiers:
    //   1. single contiguous block fits        → flat array (today's path)
    //   2. no single block, total heap allows   → paged array
    //   3. total heap (minus reserve) too small → false (caller degrades)
    bool build(nrOfLightsType logicalCount, nrOfLightsType maxDestinations) {
        free();
        identity_ = false;
        logicalCount_ = logicalCount;
        destinationCapacity_ = maxDestinations;

        size_t offsetBytes = static_cast<size_t>(logicalCount + 1) * sizeof(nrOfLightsType);
        size_t destBytes = static_cast<size_t>(maxDestinations) * sizeof(nrOfLightsType);

        // offsets_ is small (one entry per logical light + 1) and always a
        // single allocation; only destinations_ can hit the cliff.
        offsets_ = static_cast<nrOfLightsType*>(platform::alloc(offsetBytes));
        if (!offsets_) { free(); return false; }
        std::memset(offsets_, 0, offsetBytes);

        if (!allocateDestinations(destBytes, maxDestinations)) {
            free();
            return false;
        }

        destinationCount_ = 0;
        return true;
    }

    // Fill one logical entry's destinations (call sequentially, idx 0..logicalCount-1)
    void setMapping(nrOfLightsType logicalIdx, const nrOfLightsType* physicals, nrOfLightsType count) {
        if (!offsets_ || logicalIdx >= logicalCount_) return;
        offsets_[logicalIdx] = destinationCount_;
        for (nrOfLightsType i = 0; i < count && destinationCount_ < destinationCapacity_; i++) {
            writeDestination(destinationCount_, physicals[i]);
            destinationCount_++;
        }
    }

    // Call after all setMapping calls to close the last offset
    void finalize() {
        if (offsets_) {
            offsets_[logicalCount_] = destinationCount_;
        }
    }

    void free() {
        if (offsets_) { platform::free(offsets_); offsets_ = nullptr; }
        if (destinations_) { platform::free(destinations_); destinations_ = nullptr; }
        // Free pages in reverse allocation order (give the allocator the best
        // chance to coalesce the freed blocks back together).
        for (int i = pageCount_ - 1; i >= 0; i--) {
            platform::free(pages_[i]);
            pages_[i] = nullptr;
        }
        pageCount_ = 0;
        paged_ = false;
        logicalCount_ = 0;
        destinationCount_ = 0;
        destinationCapacity_ = 0;
        identity_ = true;
        overwrites_ = true;
    }

    bool hasLUT() const { return !identity_; }
    bool isPaged() const { return paged_; }
    nrOfLightsType logicalCount() const { return logicalCount_; }
    nrOfLightsType destinationCount() const { return destinationCount_; }

    // Whether each physical destination is written by at most one logical light.
    // True for every current producer (mirror, serpentine shuffle, sparse
    // box→driver) — their destinations are distinct, so blendMap can plain-copy
    // (≈4× faster than the read-add-clamp additive path). Set false only for a
    // map that intentionally folds multiple sources onto one destination (e.g.
    // future multi-layer compositing), where additive blending is required.
    bool overwrites() const { return overwrites_; }
    void setOverwrites(bool v) { overwrites_ = v; }

    // Memory accounting — actual bytes used, not capacity (destinations may be
    // over-allocated). Paging doesn't change the total.
    size_t memoryUsed() const {
        if (identity_) return 0;
        return static_cast<size_t>(logicalCount_ + 1) * sizeof(nrOfLightsType)
             + static_cast<size_t>(destinationCount_) * sizeof(nrOfLightsType);
    }

    static size_t estimateBytes(nrOfLightsType logicalCount, nrOfLightsType maxDest) {
        return static_cast<size_t>(logicalCount + 1) * sizeof(nrOfLightsType)
             + static_cast<size_t>(maxDest) * sizeof(nrOfLightsType);
    }

    // Hot-path: iterate physical destinations for a logical index.
    template<typename F>
    void forEachDestination(nrOfLightsType logicalIdx, F&& callback) const {
        if (identity_) {
            callback(logicalIdx);
            return;
        }
        if (!offsets_ || logicalIdx >= logicalCount_) return;
        nrOfLightsType start = offsets_[logicalIdx];
        nrOfLightsType end = offsets_[logicalIdx + 1];
        if (!paged_) {
            // Single contiguous array — the common case (PSRAM, small grids).
            // Byte-identical to a non-paged build; `paged_` is branch-predicted
            // not-taken so this stays the flat hot loop.
            for (nrOfLightsType i = start; i < end; i++) {
                callback(destinations_[i]);
            }
            return;
        }
        // Paged: walk the run, switching pages at each 4096 boundary. A single
        // logical entry's run may straddle a boundary, so recompute the page
        // pointer at the start and whenever the slot wraps to 0.
        for (nrOfLightsType i = start; i < end; i++) {
            const nrOfLightsType* page = pages_[i >> kPageShift];
            callback(page[i & kPageMask]);
        }
    }

private:
    // Tier the destinations allocation. Returns false only on tier 3.
    bool allocateDestinations(size_t destBytes, nrOfLightsType maxDestinations) {
        if (destBytes == 0) { paged_ = false; return true; }  // nothing to map

        // Tier 1: a single contiguous block fits. maxAllocBlock()==0 means
        // "unlimited / not meaningful" (desktop) → take the single block too, so
        // the desktop suite exercises the same flat path the device uses below
        // the fragmentation cliff.
        size_t maxBlock = platform::maxAllocBlock();
        if (maxBlock == 0 || maxBlock >= destBytes) {
            destinations_ = static_cast<nrOfLightsType*>(platform::alloc(destBytes));
            if (destinations_) { paged_ = false; return true; }
            // Fall through to paging if the single alloc lost a race.
        }

        // Tier 3 gate: refuse if total free (minus the reserve that protects
        // stacks / WiFi / HTTP) can't hold it. Cramming the heap to 100% would
        // starve those — an honest "skipped" beats a silent failure elsewhere.
        size_t freeHeap = platform::freeHeap();
        if (freeHeap != 0) {  // 0 == desktop (unlimited) → always page-able
            size_t budget = freeHeap > platform::HEAP_RESERVE
                                ? freeHeap - platform::HEAP_RESERVE : 0;
            if (budget < destBytes) return false;
        }

        // Tier 2: page it.
        int needed = static_cast<int>((maxDestinations + kPageEntries - 1) / kPageEntries);
        if (needed > kMaxPages) return false;
        for (int p = 0; p < needed; p++) {
            pages_[p] = static_cast<nrOfLightsType*>(
                platform::alloc(static_cast<size_t>(kPageEntries) * sizeof(nrOfLightsType)));
            if (!pages_[p]) {
                // Partial failure: unwind the pages we got (reverse order).
                for (int q = p - 1; q >= 0; q--) { platform::free(pages_[q]); pages_[q] = nullptr; }
                pageCount_ = 0;
                return false;
            }
        }
        pageCount_ = needed;
        paged_ = true;
        return true;
    }

    void writeDestination(nrOfLightsType slot, nrOfLightsType value) {
        if (paged_) pages_[slot >> kPageShift][slot & kPageMask] = value;
        else        destinations_[slot] = value;
    }

    bool identity_ = true;
    bool paged_ = false;
    nrOfLightsType logicalCount_ = 0;
    nrOfLightsType* offsets_ = nullptr;
    nrOfLightsType* destinations_ = nullptr;   // tier-1 single block (null when paged)
    nrOfLightsType* pages_[kMaxPages] = {};     // tier-2 pages (empty when single)
    int pageCount_ = 0;
    nrOfLightsType destinationCount_ = 0;
    nrOfLightsType destinationCapacity_ = 0;
    bool overwrites_ = true;   // single-write destinations → blendMap may copy
};

} // namespace mm
