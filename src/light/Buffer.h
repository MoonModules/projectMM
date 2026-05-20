#pragma once

#include "core/types.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstring>
#include <span>

namespace mm {

class Buffer {
public:
    Buffer() = default;
    ~Buffer() { free(); }

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    Buffer(Buffer&& other) noexcept
        : data_(other.data_), count_(other.count_), channelsPerLight_(other.channelsPerLight_) {
        other.data_ = nullptr;
        other.count_ = 0;
        other.channelsPerLight_ = 0;
    }

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            free();
            data_ = other.data_;
            count_ = other.count_;
            channelsPerLight_ = other.channelsPerLight_;
            other.data_ = nullptr;
            other.count_ = 0;
            other.channelsPerLight_ = 0;
        }
        return *this;
    }

    bool allocate(nrOfLightsType nrOfLights, uint8_t cpl) {
        free();
        size_t totalBytes = static_cast<size_t>(nrOfLights) * cpl;
        if (totalBytes == 0) return false;
        data_ = static_cast<uint8_t*>(platform::alloc(totalBytes));
        if (!data_) return false;
        count_ = nrOfLights;
        channelsPerLight_ = cpl;
        clear();
        return true;
    }

    void free() {
        if (data_) {
            platform::free(data_);
            data_ = nullptr;
        }
        count_ = 0;
        channelsPerLight_ = 0;
    }

    void clear() {
        if (data_) std::memset(data_, 0, bytes());
    }

    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }

    std::span<uint8_t> span() { return {data_, bytes()}; }
    std::span<const uint8_t> span() const { return {data_, bytes()}; }

    nrOfLightsType count() const { return count_; }
    uint8_t channelsPerLight() const { return channelsPerLight_; }
    size_t bytes() const { return static_cast<size_t>(count_) * channelsPerLight_; }

private:
    uint8_t* data_ = nullptr;
    nrOfLightsType count_ = 0;
    uint8_t channelsPerLight_ = 0;
};

} // namespace mm
