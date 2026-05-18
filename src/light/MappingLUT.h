#pragma once

#include <cstddef>
#include <cstdint>

namespace mm::light {

// CSR (Compressed Sparse Row) style mapping table.
// Maps logical pixel indices to one or more physical pixel indices.
// Supports 1:0 (skip), 1:1 (direct/shuffle), 1:N (mirror/clone).
class MappingLUT {
public:
    MappingLUT() = default;
    ~MappingLUT();

    // Move
    MappingLUT(MappingLUT&& other) noexcept;
    MappingLUT& operator=(MappingLUT&& other) noexcept;

    // No copy
    MappingLUT(const MappingLUT&) = delete;
    MappingLUT& operator=(const MappingLUT&) = delete;

    // Allocate storage. Call before setMapping. Returns false on failure.
    bool allocate(size_t logicalCount, size_t totalDestinations);
    void free();

    // Cold path: build the mapping for a logical index.
    // Must be called in order: index 0, 1, 2, ...
    // physicalIndices points to 'count' uint16_t values.
    void setMapping(size_t logicalIndex, const uint16_t* physicalIndices,
                    size_t count);

    // Finalize after all setMapping calls. Sets the sentinel offset.
    void finalize();

    // Hot path: read the mapping.
    size_t logicalCount() const { return logicalCount_; }
    size_t destinationCount(size_t logicalIndex) const;
    const uint16_t* destinations(size_t logicalIndex) const;

private:
    uint32_t* offsets_ = nullptr;       // [logicalCount + 1]
    uint16_t* destinations_ = nullptr;  // [totalDestinations]
    size_t logicalCount_ = 0;
    size_t totalDest_ = 0;
};

} // namespace mm::light
