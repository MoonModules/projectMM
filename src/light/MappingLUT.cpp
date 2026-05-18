#include "light/MappingLUT.h"
#include "platform/Alloc.h"
#include <cstring>
#include <utility>

namespace mm::light {

MappingLUT::~MappingLUT() { free(); }

MappingLUT::MappingLUT(MappingLUT&& other) noexcept
    : offsets_(other.offsets_), destinations_(other.destinations_),
      logicalCount_(other.logicalCount_), totalDest_(other.totalDest_) {
    other.offsets_ = nullptr;
    other.destinations_ = nullptr;
    other.logicalCount_ = 0;
    other.totalDest_ = 0;
}

MappingLUT& MappingLUT::operator=(MappingLUT&& other) noexcept {
    if (this != &other) {
        free();
        offsets_ = other.offsets_;
        destinations_ = other.destinations_;
        logicalCount_ = other.logicalCount_;
        totalDest_ = other.totalDest_;
        other.offsets_ = nullptr;
        other.destinations_ = nullptr;
        other.logicalCount_ = 0;
        other.totalDest_ = 0;
    }
    return *this;
}

bool MappingLUT::allocate(size_t logicalCount, size_t totalDestinations) {
    free();
    if (logicalCount == 0) return true;

    void* op = platform::alloc((logicalCount + 1) * sizeof(uint32_t));
    if (!op) return false;

    void* dp = platform::alloc(totalDestinations * sizeof(uint16_t));
    if (!dp && totalDestinations > 0) {
        platform::free(op);
        return false;
    }

    offsets_ = static_cast<uint32_t*>(op);
    destinations_ = static_cast<uint16_t*>(dp);
    logicalCount_ = logicalCount;
    totalDest_ = totalDestinations;

    std::memset(offsets_, 0, (logicalCount + 1) * sizeof(uint32_t));
    if (destinations_ && totalDestinations > 0) {
        std::memset(destinations_, 0, totalDestinations * sizeof(uint16_t));
    }

    return true;
}

void MappingLUT::free() {
    if (offsets_) { platform::free(offsets_); offsets_ = nullptr; }
    if (destinations_) { platform::free(destinations_); destinations_ = nullptr; }
    logicalCount_ = 0;
    totalDest_ = 0;
}

void MappingLUT::setMapping(size_t logicalIndex, const uint16_t* physicalIndices,
                             size_t count) {
    if (logicalIndex >= logicalCount_) return;

    // Set the offset for this logical index
    uint32_t start = (logicalIndex == 0) ? 0 : offsets_[logicalIndex];
    offsets_[logicalIndex] = start;

    // Copy destination indices
    for (size_t i = 0; i < count; ++i) {
        if (start + i < totalDest_) {
            destinations_[start + i] = physicalIndices[i];
        }
    }

    // Set the next offset (used by the next setMapping call or finalize)
    offsets_[logicalIndex + 1] = static_cast<uint32_t>(start + count);
}

void MappingLUT::finalize() {
    // offsets_[logicalCount_] should already be set by the last setMapping
}

size_t MappingLUT::destinationCount(size_t logicalIndex) const {
    if (logicalIndex >= logicalCount_) return 0;
    return offsets_[logicalIndex + 1] - offsets_[logicalIndex];
}

const uint16_t* MappingLUT::destinations(size_t logicalIndex) const {
    if (logicalIndex >= logicalCount_) return nullptr;
    return &destinations_[offsets_[logicalIndex]];
}

} // namespace mm::light
