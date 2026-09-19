#pragma once

#include <atomic>

namespace mm {

/// A non-blocking latch guarding a resource two threads reach, in this codebase the sender one core drains while another streams into it.
///
/// @moreinfo
///
/// ## Only try-acquire, never a blocking lock
///
/// The hot-path rule forbids a render or encode thread blocking on a peer, so a caller that loses the race skips its slot rather than waiting.
/// That single constraint is what lets this be a test-and-set on the one type the standard guarantees lock-free, rather than an operating-system mutex.
/// With no waiting there is nothing to sleep on, nothing to wake, and no priority to inherit.
/// So it costs one atomic read-modify-write with no system call, and needs no lifecycle.
///
/// It is not recursive: a thread already holding it must not re-acquire, which the test would refuse.
///
/// ## Why it lives in core rather than the platform layer
///
/// It is portable code with no hardware backing, so it fails that layer's charter, which is the one place hardware interfaces are allowed.
/// Domain-neutral and reusable, so any module coordinating two threads over one resource can take it.
class TryLock {
public:
    /// Take the latch when it is free, false when another thread holds it.
    bool tryAcquire() { return !flag_.test_and_set(std::memory_order_acquire); }
    /// Give it back.
    void release() { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

/// A scope guard releasing on exit and doing nothing when the latch was busy, the standard try-to-lock shape.
class LockGuard {
public:
    /// Tries the latch once; test the guard to learn whether it took.
    explicit LockGuard(TryLock& l) : lock_(l), held_(l.tryAcquire()) {}
    /// Releases only what it took.
    ~LockGuard() { if (held_) lock_.release(); }
    /// Whether this guard holds the latch.
    explicit operator bool() const { return held_; }

    /// Non-copyable: two guards would release one latch twice.
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    TryLock& lock_;
    bool held_;
};

}  // namespace mm
