#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mm {

/// The ring itself, sized at compile time by its element type and capacity.
template <typename T, uint32_t N>
/// The textbook single-producer single-consumer ring, where one thread pushes and one pops, never the same thread for both roles.
///
/// The capacity must be a power of two, indices wrapping by masking.
/// One slot is sacrificed, so a full ring is distinguishable from an empty one without a count.
///
/// @moreinfo
///
/// ## Why neither side ever locks
///
/// Each index is written by exactly one side.
/// The release store on the writer's index paired with the acquire load on the reader's makes the element data visible before the index moves, which is the whole correctness argument.
///
/// ## Overflow drops the newest
///
/// A push accepts what fits and reports how much.
/// Dropping the oldest instead would need the producer to advance the consumer's index, a second writer on it, which breaks the single-writer invariant the lock-freedom rests on.
///
/// For the audio capture the trade is right anyway.
/// Overflow happens when the consumer stalls or renders below one block per fill, latency then pinning at the ring depth, and the backlog drains once it catches up.
class SpscRing {
    static_assert((N & (N - 1)) == 0 && N > 1, "capacity must be a power of two");

public:
    /// The producer side, returning how many elements were accepted, fewer than asked when the ring is near full.
    size_t push(const T* src, size_t count) {
        if (src == nullptr) return 0;
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        const uint32_t free = N - 1 - (head - tail);
        const size_t n = count < free ? count : free;
        for (size_t i = 0; i < n; i++) buf_[(head + i) & (N - 1)] = src[i];
        head_.store(head + static_cast<uint32_t>(n), std::memory_order_release);
        return n;
    }

    /// The consumer side, returning how many elements were read.
    size_t pop(T* dst, size_t max) {
        if (dst == nullptr) return 0;
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        const uint32_t avail = head - tail;
        const size_t n = max < avail ? max : avail;
        for (size_t i = 0; i < n; i++) dst[i] = buf_[(tail + i) & (N - 1)];
        tail_.store(tail + static_cast<uint32_t>(n), std::memory_order_release);
        return n;
    }

    /// How many elements are queued as the consumer sees it, approximate under concurrency and exact when one side is idle.
    size_t size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    T buf_[N];
    // Free-running indices that wrap by masking; unsigned overflow is defined, so the subtraction stays correct across it.
    std::atomic<uint32_t> head_{0};   ///< written only by the producer
    std::atomic<uint32_t> tail_{0};   ///< written only by the consumer
};

}  // namespace mm
