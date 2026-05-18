# MoonDeck Script Reference

## build_desktop

Build the desktop target using CMake.

```bash
uv run scripts/build_desktop.py
```

Runs `cmake -B build -DCMAKE_BUILD_TYPE=Release` then `cmake --build build`
from the project root. Requires CMake installed.

## run_desktop

Run the desktop executable.

```bash
uv run scripts/run_desktop.py
```

Runs `./build/mmv3`. Build first with `build_desktop`.

## test_desktop

Run the desktop test suite.

```bash
uv run scripts/test_desktop.py
```

Runs `cmake --build build --target test`. Build first with `build_desktop`.

## check_platform_boundary

Verify that platform-specific code stays inside `src/platform/`.

```bash
uv run scripts/check_platform_boundary.py
```

Scans all `.h`, `.hpp`, `.cpp`, `.c` files outside `src/platform/` for
forbidden includes (`esp_*`, `freertos/*`, `driver/*`, `SDL.h`, etc.)
and platform-specific `#ifdef`s. Exits with code 1 if violations found.

## build_esp32

Build for an ESP32 chip target.

```bash
uv run scripts/build_esp32.py --env esp32s3
```

Runs `idf.py set-target <env>` then `idf.py build` in the `esp32/`
directory. Requires ESP-IDF installed and sourced.

## flash_esp32

Flash firmware to an ESP32 device.

```bash
uv run scripts/flash_esp32.py --env esp32s3 --port /dev/tty.usbserial-0001
```

Runs `idf.py flash -p <port>` in the `esp32/` directory.

## monitor_esp32

Monitor serial output from an ESP32 device.

```bash
uv run scripts/monitor_esp32.py --port /dev/tty.usbserial-0001
```

Runs `idf.py monitor -p <port>`. Press Ctrl+C to stop.
