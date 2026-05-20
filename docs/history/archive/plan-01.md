# Plan: Core Pipeline on Desktop — Lights on Panel via ArtNet

## Context

Item 1 from docs/plan.md. This is the first implementation commit of projectMM v3. No source code exists yet — only architecture docs and promoted specs. The goal is a working pipeline: GridLayout → RainbowEffect → ArtNetSendDriver → lights visible on a real hub75 panel via ArtNet receiver, running on macOS desktop.

Agreed simplifications (from product owner):
- **No MappingLUT** — Grid is 1:1 unshuffled, no mapping table needed
- **No DriverGroup buffer** — reads directly from Layer buffer
- **No BlendMap** — single layer, 1:1 unshuffled
- **LightConfig minimal** — RGB only (channelsPerLight=3)
- **EffectBase** — start with thin class (may absorb into Layer later)
- **Scheduler drives everything**

## File Tree

```
CMakeLists.txt                              # Root: C++20, -Wall -Wextra -Werror, core + platform libs, test target
src/
  platform/
    platform.h                              # mm::platform API: millis, micros, alloc, free, UdpSocket
    desktop/
      platform_desktop.cpp                  # std::chrono, std::malloc, BSD sockets
  core/
    types.h                                 # nrOfLightsType (uint32_t), lengthType (int16_t), CoordCallback
    color.h                                 # hsvToRgb, scale8 — constexpr, integer, no floats
    Control.h                               # ControlDescriptor (<16B on ESP32), ControlList<N>
    MoonModule.h                            # Base class: lifecycle, controls, name, parent
    Scheduler.h                             # Module registry, tick(), elapsed(), loop/20ms/1s dispatch
  light/
    Buffer.h                                # uint8_t* buffer, move-only, allocate/free/clear/span
    LayoutGroup.h                           # Groups layouts, forEachCoord with index offset
    GridLayout.h                            # width×height×depth grid, row-major coordinates
    EffectBase.h                            # Thin accessors to parent Layer
    Layer.h                                 # Owns buffer + effects list, render = run effects in order
    RainbowEffect.h                        # Diagonal rainbow, BPM speed control
    DriverGroup.h                           # Groups drivers, passes layer buffer to each
    ArtNetSendDriver.h                      # ArtNet OpDmx packets over UDP, universe splitting, FPS limit
  main.cpp                                  # Wire pipeline, run scheduler loop
test/
  CMakeLists.txt                            # Test executable
  doctest.h                                 # Vendored header-only test framework
  test_color.cpp                            # hsvToRgb at cardinal hues, scale8
  test_buffer.cpp                           # Allocate, clear, move, double-free safety
  test_moonmodule.cpp                       # Lifecycle, control binding
  test_grid_layout.cpp                      # Coordinate iteration, row-major order, 3D
  test_rainbow.cpp                          # Buffer contains expected hsvToRgb values
  test_artnet_packet.cpp                    # Header format, byte order, universe splitting
  test_pipeline.cpp                         # Full pipeline: grid→layer→rainbow→artnet packets
```

17 source files, 7 test files, 2 CMake files. All MoonModules are single `.h` files. Only `platform_desktop.cpp` is a `.cpp` file.

## Implementation Steps

### Step 1: CMake + Platform + Types

Files: `CMakeLists.txt`, `src/platform/platform.h`, `src/platform/desktop/platform_desktop.cpp`, `src/core/types.h`, `test/CMakeLists.txt`, `test/doctest.h`

- Root CMake: C++20, warnings as errors, `mm_core` (INTERFACE lib — all headers), `mm_platform` (desktop .cpp), `mmv3` executable, test target
- Platform API in `mm::platform`: `millis()`, `micros()`, `alloc(size)`, `free(ptr)`, `UdpSocket` class (open/send/close)
- Desktop: `std::chrono::steady_clock`, `std::malloc`/`std::free`, BSD sockets (`socket`, `sendto`, `inet_pton`)
- Types: `nrOfLightsType = uint32_t`, `lengthType = int16_t` (desktop uses larger types)
- Vendor `doctest.h` into `test/`

### Step 2: Color Math

Files: `src/core/color.h`, `test/test_color.cpp`

```cpp
namespace mm {
    struct RGB { uint8_t r, g, b; };
    constexpr RGB hsvToRgb(uint8_t h, uint8_t s, uint8_t v);  // 6-sector integer
    constexpr uint8_t scale8(uint8_t val, uint8_t scale);
}
```

RGB struct is a return type only — buffers remain `uint8_t*`. Tests: h=0→red, h=85→green, h=170→blue, s=0→white, v=0→black, scale8(255,128)≈127.

### Step 3: Control + MoonModule

Files: `src/core/Control.h`, `src/core/MoonModule.h`, `test/test_moonmodule.cpp`

```cpp
namespace mm {
    enum class ControlType : uint8_t { Uint8, Uint16, Bool, Text };

    struct ControlDescriptor {       // <16 bytes on ESP32 (32-bit pointers)
        void* ptr;                   // pointer to class variable
        const char* name;            // flash/constexpr string
        ControlType type;
        uint8_t min, max;
    };

    template<size_t Capacity = 8>
    struct ControlList { ... };

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
        const char* name() const;
        MoonModule* parent() const;
        void setParent(MoonModule* p);
    protected:
        ControlList<8> controls_;
        template<typename T>
        void addControl(const char* name, T& var, T min = {}, T max = {});
    private:
        const char* name_ = nullptr;
        MoonModule* parent_ = nullptr;
    };
}
```

Tests: concrete subclass with uint8_t control, verify pointer binding, lifecycle calls.

### Step 4: Buffer

Files: `src/light/Buffer.h`, `test/test_buffer.cpp`

```cpp
namespace mm {
    class Buffer {
    public:
        bool allocate(nrOfLightsType nrOfLights, uint8_t channelsPerLight);
        void free();
        void clear();
        uint8_t* data();
        std::span<uint8_t> span();
        nrOfLightsType count() const;
        uint8_t channelsPerLight() const;
        size_t bytes() const;
        // Move-only
    };
}
```

Tests: allocate 256×3, verify bytes/count/channelsPerLight, clear zeros, move leaves source null, double-free safe.

### Step 5: LayoutGroup + GridLayout

Files: `src/light/LayoutGroup.h`, `src/light/GridLayout.h`, `test/test_grid_layout.cpp`

```cpp
namespace mm {
    using CoordCallback = void(*)(void* ctx, nrOfLightsType idx, lengthType x, lengthType y, lengthType z);

    class LayoutBase : public MoonModule {
        virtual nrOfLightsType lightCount() const = 0;
        virtual void forEachCoord(CoordCallback cb, void* ctx) const = 0;
    };

    class LayoutGroup : public MoonModule {
        void addLayout(LayoutBase* layout);
        nrOfLightsType totalLightCount() const;
        void forEachCoord(CoordCallback cb, void* ctx) const;
    };

    class GridLayout : public LayoutBase {
        lengthType width = 16, height = 16, depth = 1;
    };
}
```

Tests: 4×4×1 yields 16 coords row-major, 2×2×2 yields 8, totalLightCount with multiple layouts.

### Step 6: Scheduler

Files: `src/core/Scheduler.h`

```cpp
namespace mm {
    class Scheduler {
    public:
        void addModule(MoonModule* mod);
        void setup();     // setup → onBuildControls → onAllocateMemory on each
        void tick();      // loop on all, loop20ms/loop1s when due
        void teardown();
        uint32_t elapsed() const;
    private:
        std::array<MoonModule*, 32> modules_{};
        uint8_t moduleCount_ = 0;
    };
}
```

Tested via integration test. Fixed-capacity array, no heap.

### Step 7: Layer + EffectBase + RainbowEffect

Files: `src/light/EffectBase.h`, `src/light/Layer.h`, `src/light/RainbowEffect.h`, `test/test_rainbow.cpp`

```cpp
namespace mm {
    class EffectBase : public MoonModule {
        // Accessors delegate to parent Layer
        uint8_t* buffer();
        lengthType width() const;
        lengthType height() const;
        // ...
    };

    class Layer : public MoonModule {
        void setLayoutGroup(LayoutGroup* lg);
        void addEffect(EffectBase* effect);
        void onAllocateMemory() override;  // allocate buffer from layout dims
        void loop() override;              // run each effect's loop()
        Buffer& buffer();
        lengthType width() const;
        // elapsed_ updated from platform::millis() at start of loop()
    };

    class RainbowEffect : public EffectBase {
        uint8_t speed = 60;  // BPM
        void loop() override;
        // hue = (x + y) * scale + elapsed_phase, hsvToRgb(hue, 255, 255)
    };
}
```

Tests: 4×4 grid + rainbow at elapsed=0, verify pixel (0,0) matches hsvToRgb(0,255,255), buffer non-zero.

### Step 8: DriverGroup + ArtNetSendDriver

Files: `src/light/DriverGroup.h`, `src/light/ArtNetSendDriver.h`, `test/test_artnet_packet.cpp`

```cpp
namespace mm {
    class DriverBase : public MoonModule {
        virtual void setSourceBuffer(Buffer* buf) = 0;
    };

    class DriverGroup : public MoonModule {
        void addDriver(DriverBase* driver);
        void setLayer(Layer* layer);  // reads layer buffer directly
        void loop() override;         // calls each driver's loop()
    };

    class ArtNetSendDriver : public DriverBase {
        char ip[16] = "192.168.1.70";
        uint16_t universeStart = 0;
        uint8_t fps = 50;
        // buildPacket(buf, universe, data, len) — testable without network
        // sendUniverse() calls buildPacket then socket.send
    };
}
```

`buildPacket()` is a separate method for testability (writes to byte array, no network I/O).

Tests: header "Art-Net\0", OpCode 0x5000 (LE), ProtVer 14 (BE), sequence, universe (LE), length (BE), data at offset 18. Universe splitting: 256 RGB lights → 2 universes.

ArtNet byte order details:
- OpCode at offset 8: little-endian (0x00, 0x50)
- ProtVer at offset 10: big-endian (0x00, 0x0e)
- Universe at offset 14: little-endian
- Length at offset 16: big-endian

### Step 9: main.cpp + Integration Test

Files: `src/main.cpp`, `test/test_pipeline.cpp`

```cpp
int main() {
    mm::Scheduler scheduler;
    mm::LayoutGroup layoutGroup;
    mm::GridLayout grid;
    layoutGroup.addLayout(&grid);

    mm::Layer layer;
    layer.setLayoutGroup(&layoutGroup);
    mm::RainbowEffect rainbow;
    layer.addEffect(&rainbow);

    mm::DriverGroup driverGroup;
    driverGroup.setLayer(&layer);
    mm::ArtNetSendDriver artnet;
    driverGroup.addDriver(&artnet);

    scheduler.addModule(&layoutGroup);
    scheduler.addModule(&grid);
    scheduler.addModule(&layer);
    scheduler.addModule(&rainbow);
    scheduler.addModule(&driverGroup);
    scheduler.addModule(&artnet);

    scheduler.setup();
    while (true) scheduler.tick();
    scheduler.teardown();
}
```

All objects stack-allocated. Only the Buffer inside Layer uses `platform::alloc`.

Integration test: create full pipeline, run a few ticks, use `buildPacket()` to verify ArtNet packets contain non-zero rainbow data and correct universe count.

## Verification

1. `cmake -B build && cmake --build build` — zero warnings
2. `cd build && ctest --output-on-failure` — all 7 test files pass
3. `./build/mmv3` — runs, sends ArtNet packets to 192.168.1.70
4. Lights visible on hub75 panel via ArtNet receiver — animated rainbow
5. Platform boundary check: no `#ifdef` or platform includes outside `src/platform/`
