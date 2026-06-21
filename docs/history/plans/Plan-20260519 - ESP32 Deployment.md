# Plan: ESP32 Deployment

## Context

Item 2 from docs/plan.md. The core pipeline works on desktop (GridLayout → RainbowEffect → ArtNet → lights on panel). Now deploy the same pipeline on ESP32dev to prove the platform abstraction works. No System module — that comes after the UI.

## What needs to happen

1. ESP32 platform implementations (timing, alloc, UDP socket)
2. ESP-IDF project wrapper (`esp32/`)
3. Refactor `src/main.cpp` to share pipeline wiring between desktop and ESP32 entry points
4. WiFi init in ESP32 entry point
5. FreeRTOS watchdog yield

## Files

```
src/platform/
  platform.h                              # MODIFY: add yield()
  desktop/
    platform_desktop.cpp                   # MODIFY: add yield(), move UdpSocket::close to use ::close directly
    main_desktop.cpp                       # NEW: int main() with SIGINT handler
  esp32/
    platform_esp32.cpp                     # NEW: esp_timer, heap_caps_malloc, lwIP sockets, vTaskDelay
src/
  main.cpp                                 # MODIFY: extract mm_main(volatile bool&), add platform::yield()
esp32/
  CMakeLists.txt                           # NEW: ESP-IDF project root
  main/
    CMakeLists.txt                         # NEW: idf_component_register
    main.cpp                               # NEW: app_main, WiFi init, calls mm_main
    wifi_credentials.example.h             # NEW: template for SSID/password
CMakeLists.txt                             # MODIFY: add main_desktop.cpp to mmv3 executable
.gitignore                                 # MODIFY: add esp32 build artifacts, wifi_credentials.h
```

## Implementation Steps

### Step 1: Add `platform::yield()` and refactor entry points

Add `void yield()` to `platform.h`. Desktop: `sched_yield()` or no-op. 

Refactor `src/main.cpp`: extract `void mm_main(volatile bool& keepRunning)` with the pipeline wiring + scheduler loop + `platform::yield()` call each iteration. No signal handling, no `int main()`.

Create `src/platform/desktop/main_desktop.cpp`:
```cpp
#include <csignal>
extern void mm_main(volatile bool& keepRunning);
static volatile bool running = true;
static void signalHandler(int) { running = false; }
int main() {
    std::signal(SIGINT, signalHandler);
    mm_main(running);
    return 0;
}
```

Update root `CMakeLists.txt`:
```cmake
add_executable(mmv3 src/main.cpp src/platform/desktop/main_desktop.cpp)
```

Verify: desktop build + tests still pass.

### Step 2: ESP32 platform implementation

Create `src/platform/esp32/platform_esp32.cpp`:
- `millis()` → `esp_timer_get_time() / 1000`
- `micros()` → `esp_timer_get_time()`
- `alloc()` → `heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` with fallback to `MALLOC_CAP_8BIT`
- `free()` → `heap_caps_free()`
- `UdpSocket` → same BSD socket code as desktop but with `lwip/sockets.h`
- `yield()` → `vTaskDelay(pdMS_TO_TICKS(1))`

### Step 3: ESP-IDF project wrapper

`esp32/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mmv3)
```

`esp32/main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "main.cpp" "../../src/main.cpp" "../../src/platform/esp32/platform_esp32.cpp"
    INCLUDE_DIRS "../../src"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
```

### Step 4: ESP32 entry point

`esp32/main/main.cpp`:
- NVS init
- WiFi STA connect (hardcoded credentials from `wifi_credentials.h`)
- Wait for IP
- Call `mm_main(running)`

`esp32/main/wifi_credentials.example.h`:
```cpp
#pragma once
#define WIFI_SSID "your_ssid"
#define WIFI_PASS "your_password"
```

Actual `wifi_credentials.h` is gitignored.

### Step 5: sdkconfig.defaults + .gitignore

`esp32/sdkconfig.defaults`:
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`
- `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_USE_CAPS_ALLOC=y`
- `CONFIG_COMPILER_CXX_EXCEPTIONS=n`
- `CONFIG_LWIP_SO_REUSE=y`

`.gitignore` additions: `esp32/build/`, `esp32/sdkconfig`, `esp32/sdkconfig.old`, `esp32/main/wifi_credentials.h`

## Verification

1. `cmake --build build` — desktop still builds, zero warnings
2. `cd build && ctest --output-on-failure` — all tests pass
3. `./build/test/mm_scenarios` — scenario passes
4. `python scripts/check/check_platform_boundary.py` — passes
5. `cd esp32 && idf.py set-target esp32 && idf.py build` — ESP32 builds
6. Flash + monitor: WiFi connects, serial shows "mmv3 running", ArtNet packets arrive at receiver
7. Lights visible on hub75 panel from ESP32

## Notes

- WiFi credentials are hardcoded for this deployment. Proper WiFi MoonModule comes later.
- Grid defaults to 128x128 (fits in PSRAM). For ESP32 without PSRAM, pass smaller dimensions.
- ESP-IDF v5.1+ required for C++20 support.
- The `volatile bool` for keepRunning is sufficient — no signal handler on ESP32, no cross-thread access.
