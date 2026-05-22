# Plan: SystemModule + NetworkModule (Items 9+10)

## Context

Add system diagnostics and network connectivity as MoonModules. SystemModule shows heap/fps/uptime/deviceName. NetworkModule manages Ethernet → WiFi STA → WiFi AP cascade with automatic fallback. Both appear as cards in the web UI.

Requires new control types (ReadOnly, Select, Progress) and platform functions (getMacAddress, WiFi, Ethernet, mDNS).

## Phase 1: New Control Types

Add three control types to support SystemModule and NetworkModule.

**Control.h** — add to ControlType enum:
- `ReadOnly` — display-only text (ptr → char buffer, max = bufSize)
- `Select` — dropdown (ptr → uint8_t index, options stored via aux field)
- `Progress` — bar with value/total (ptr → uint32_t value, aux = total)

Add `uint32_t aux = 0` to ControlDescriptor (Progress total, Select options pointer).

Add methods: `addReadOnly()`, `addSelect()`, `addProgress()`.

**HttpServerModule.h** — serialize new types in writeControls:
- ReadOnly: `{"name":"fps","type":"display","value":"42"}`
- Select: `{"name":"addressing","type":"select","value":0,"options":["DHCP","Static"]}`
- Progress: `{"name":"freeHeap","type":"progress","value":180000,"total":320000}`

**handleSetControl** — after setting any value, also clear+rebuild controls on the target module (for dynamic onBuildControls). ReadOnly and Progress are skipped (read-only).

**app.js** — render new types:
- `display`: read-only span
- `select`: `<select>` element with options
- `progress`: `<progress>` element with percentage label

**Files**: `src/core/Control.h`, `src/core/HttpServerModule.h`, `src/ui/app.js`

## Phase 2: SystemModule

**src/core/SystemModule.h** — new MoonModule:
- `deviceName` (Text, default MM-XXXX from MAC)
- Dynamic (loop1s): uptime, fps, tickTimeUs (ReadOnly), freeHeap, freeInternal (Progress)
- Static: chip, idfVersion (ReadOnly)
- Needs `setScheduler()` for fps/tickTimeUs access

**Platform additions** (`src/platform/platform.h`):
- `getMacAddress(uint8_t[6])` — ESP32: `esp_efuse_mac_get_default()`, desktop: stable fake
- `totalHeap()` — ESP32: `heap_caps_get_total_size()`, desktop: 0
- `totalInternalHeap()` — same for internal
- `chipModel()` — ESP32: `esp_chip_info()`, desktop: "desktop"
- `sdkVersion()` — ESP32: `esp_get_idf_version()`, desktop: compiler version

**Registration**: first module in scheduler (before everything else).

**Factory**: `ModuleFactory::registerType<SystemModule>("SystemModule")`

**Files**: `src/core/SystemModule.h` (new), `src/platform/platform.h`, `src/platform/esp32/platform_esp32.cpp`, `src/platform/desktop/platform_desktop.cpp`, `src/main.cpp`

## Phase 3: Platform Network Abstraction

Add network functions to `src/platform/platform.h`:

```text
bool ethInit();
bool ethConnected();
void ethGetIP(char* buf, size_t len);

bool wifiStaInit(const char* ssid, const char* password);
bool wifiStaConnected();
void wifiStaGetIP(char* buf, size_t len);
void wifiStaStop();

bool wifiApInit(const char* ssid, const char* ip);
bool wifiApConnected();
void wifiApStop();

bool mdnsInit(const char* deviceName);
void mdnsStop();
```

ESP32: implement using ESP-IDF APIs. Move Ethernet init logic from `esp32/main/main.cpp` into `platform::ethInit()` (non-blocking, no `xEventGroupWaitBits`).

Desktop: all return false / no-op.

**Files**: `src/platform/platform.h`, `src/platform/esp32/platform_esp32.cpp`, `src/platform/desktop/platform_desktop.cpp`

## Phase 4: NetworkModule

**src/core/NetworkModule.h** — new MoonModule:
- Controls: ssid, password (Text), addressing (Select: DHCP/Static), dynamic IP fields, dns, status (ReadOnly)
- Priority cascade in setup(): ethInit → wifiStaInit → wifiApInit(deviceName, "4.3.2.1")
- loop1s(): monitor connections, cascade up/down, AP shutdown delay (10s)
- Reads deviceName from SystemModule (via setSystemModule pointer)
- After network transitions: `scheduler_->rebuild()` to re-evaluate light buffer allocation

**Dynamic controls**: onBuildControls checks `addressing_` — Static shows ip/gateway/subnet/dns, DHCP hides them.

**ESP32 only**: guarded by `#ifdef ESP_PLATFORM` in mm_main. Compiles on desktop (platform stubs) but not instantiated.

**Registration order**: SystemModule, NetworkModule, LayoutGroup, Layer, DriverGroup, HttpServerModule.

**Files**: `src/core/NetworkModule.h` (new), `src/main.cpp`

## Phase 5: Clean up esp32/main.cpp

- Remove `eth_init()`, `eth_event_handler()`, `ethEventGroup` from `esp32/main/main.cpp`
- Remove blocking wait
- `app_main()` becomes: NVS init → `mm_main()`
- Ethernet is now handled by NetworkModule via `platform::ethInit()`

**Files**: `esp32/main/main.cpp`

## Phase 6: Tests + Docs

**Tests**:
- `test/test_moonmodule.cpp` — ReadOnly, Select, Progress control types
- `test/test_system_module.cpp` (new) — MAC-to-deviceName conversion
- Existing scenarios must still pass

**Docs**:
- `docs/moonmodules/core/Control.md` — document new types
- `docs/moonmodules/core/SystemModule.md` — mark implemented
- `docs/moonmodules/core/NetworkModule.md` — mark implemented
- `docs/testing.md` — add test entries
- `docs/plan.md` — remove items 9+10

## Verification

1. Desktop build + all tests pass
2. Desktop: System card shows uptime/fps/heap/deviceName in UI
3. ESP32 build passes
4. ESP32 with Ethernet: connects, System+Network cards visible in UI
5. ESP32 without Ethernet: falls back to WiFi STA or AP
6. WiFi credential injection via REST API works
7. Platform boundary check passes
8. Pre-commit checklist (8 steps)

## Files Summary

```text
src/core/Control.h              # new types: ReadOnly, Select, Progress
src/core/SystemModule.h          # NEW
src/core/NetworkModule.h         # NEW
src/core/HttpServerModule.h      # serialize new types, dynamic onBuildControls
src/platform/platform.h          # getMacAddress, totalHeap, network functions
src/platform/esp32/platform_esp32.cpp   # implement all new platform functions
src/platform/desktop/platform_desktop.cpp # stubs
src/main.cpp                     # register + create SystemModule, NetworkModule
src/ui/app.js                    # render display/select/progress types
esp32/main/main.cpp              # strip Ethernet init
test/test_moonmodule.cpp         # new control type tests
test/test_system_module.cpp      # NEW: MAC-to-name test
docs/moonmodules/core/Control.md
docs/testing.md
```
