#pragma once

#include "light/Pixel.h"
#include "platform/Alloc.h"
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

namespace mm::light {

class Buffer {
public:
    Buffer() = default;
    ~Buffer() { free(); }

    // Move
    Buffer(Buffer&& other) noexcept
        : data_(other.data_), count_(other.count_) {
        other.data_ = nullptr;
        other.count_ = 0;
    }

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            free();
            data_ = other.data_;
            count_ = other.count_;
            other.data_ = nullptr;
            other.count_ = 0;
        }
        return *this;
    }

    // No copy
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    bool allocate(size_t count) {
        free();
        if (count == 0) return true;
        void* p = platform::alloc(count * sizeof(RGB));
        if (!p) return false;
        data_ = static_cast<RGB*>(p);
        count_ = count;
        clear();
        return true;
    }

    void free() {
        if (data_) {
            platform::free(data_);
            data_ = nullptr;
            count_ = 0;
        }
    }

    void clear() {
        if (data_) std::memset(data_, 0, bytes());
    }

    void fill(RGB color) {
        for (size_t i = 0; i < count_; ++i) data_[i] = color;
    }

    RGB& operator[](size_t i) { return data_[i]; }
    const RGB& operator[](size_t i) const { return data_[i]; }

    std::span<RGB> pixels() { return {data_, count_}; }
    std::span<const RGB> pixels() const { return {data_, count_}; }

    size_t count() const { return count_; }
    size_t bytes() const { return count_ * sizeof(RGB); }

private:
    RGB* data_ = nullptr;
    size_t count_ = 0;
};

} // namespace mm::light
