#pragma once

#include "core/Control.h"

namespace mm {

class MoonModule {
public:
    virtual ~MoonModule() = default;

    virtual void setup() {}
    virtual void loop() {}
    virtual void loop20ms() {}
    virtual void loop1s() {}
    virtual void teardown() {}
    virtual void onBuildControls() {}
    virtual void onAllocateMemory() {}

    const char* name() const { return name_; }
    void setName(const char* n) { name_ = n; }

    MoonModule* parent() const { return parent_; }
    void setParent(MoonModule* p) { parent_ = p; }

    ControlList<8>& controls() { return controls_; }
    const ControlList<8>& controls() const { return controls_; }

protected:
    ControlList<8> controls_;

private:
    const char* name_ = nullptr;
    MoonModule* parent_ = nullptr;
};

} // namespace mm
