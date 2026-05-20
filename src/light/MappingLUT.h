#pragma once

#include "core/types.h"
#include "platform/platform.h"

#include <cstring>

namespace mm {

class MappingLUT {
public:
    MappingLUT() = default;
    ~MappingLUT() { free(); }

    MappingLUT(const MappingLUT&) = delete;
    MappingLUT& operator=(const MappingLUT&) = delete;

    // Fast path: logical == physical, no table needed
    void setIdentity(nrOfLightsType count) {
        free();
        identity_ = true;
        logicalCount_ = count;
    }

    // Allocate CSR arrays for 1:N mapping
    bool build(nrOfLightsType logicalCount, nrOfLightsType maxDestinations) {
        free();
        identity_ = false;
        logicalCount_ = logicalCount;
        destinationCapacity_ = maxDestinations;

        size_t offsetBytes = static_cast<size_t>(logicalCount + 1) * sizeof(nrOfLightsType);
        size_t destBytes = static_cast<size_t>(maxDestinations) * sizeof(nrOfLightsType);

        offsets_ = static_cast<nrOfLightsType*>(platform::alloc(offsetBytes));
        destinations_ = static_cast<nrOfLightsType*>(platform::alloc(destBytes));

        if (!offsets_ || !destinations_) {
            free();
            return false;
        }

        std::memset(offsets_, 0, offsetBytes);
        destinationCount_ = 0;
        return true;
    }

    // Fill one logical entry's destinations (call sequentially, idx 0..logicalCount-1)
    void setMapping(nrOfLightsType logicalIdx, const nrOfLightsType* physicals, nrOfLightsType count) {
        if (!offsets_ || !destinations_) return;

        offsets_[logicalIdx] = destinationCount_;
        for (nrOfLightsType i = 0; i < count && destinationCount_ < destinationCapacity_; i++) {
            destinations_[destinationCount_++] = physicals[i];
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
        logicalCount_ = 0;
        destinationCount_ = 0;
        destinationCapacity_ = 0;
        identity_ = true;
    }

    bool hasLUT() const { return !identity_; }
    nrOfLightsType logicalCount() const { return logicalCount_; }
    nrOfLightsType destinationCount() const { return destinationCount_; }

    // Memory accounting
    size_t memoryUsed() const {
        if (identity_) return 0;
        // Actual bytes used (not capacity — destinations may be over-allocated)
        return static_cast<size_t>(logicalCount_ + 1) * sizeof(nrOfLightsType)
             + static_cast<size_t>(destinationCount_) * sizeof(nrOfLightsType);
    }

    static size_t estimateBytes(nrOfLightsType logicalCount, nrOfLightsType maxDest) {
        return static_cast<size_t>(logicalCount + 1) * sizeof(nrOfLightsType)
             + static_cast<size_t>(maxDest) * sizeof(nrOfLightsType);
    }

    // Hot-path: iterate physical destinations for a logical index
    template<typename F>
    void forEachDestination(nrOfLightsType logicalIdx, F&& callback) const {
        if (identity_) {
            callback(logicalIdx);
            return;
        }
        if (!offsets_ || !destinations_ || logicalIdx >= logicalCount_) return;
        nrOfLightsType start = offsets_[logicalIdx];
        nrOfLightsType end = offsets_[logicalIdx + 1];
        for (nrOfLightsType i = start; i < end; i++) {
            callback(destinations_[i]);
        }
    }

private:
    bool identity_ = true;
    nrOfLightsType logicalCount_ = 0;
    nrOfLightsType* offsets_ = nullptr;
    nrOfLightsType* destinations_ = nullptr;
    nrOfLightsType destinationCount_ = 0;
    nrOfLightsType destinationCapacity_ = 0;
};

} // namespace mm
