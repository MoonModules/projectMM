#pragma once

#include "core/MoonModule.h"
#include "light/Pixel.h"
#include <span>

namespace mm::light {

// Light-domain MoonModule subclass for drivers.
// Receives a read-only view of the DriverGroup's output buffer.
class DriverBase : public MoonModule {
public:
    void setOutputBuffer(std::span<const RGB> buf) { outputBuffer_ = buf; }

protected:
    std::span<const RGB> outputBuffer() const { return outputBuffer_; }

private:
    std::span<const RGB> outputBuffer_;
};

} // namespace mm::light
