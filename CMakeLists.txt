cmake_minimum_required(VERSION 3.20)
project(projectMM LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warning flags differ between GCC/Clang and MSVC. Gate by compiler so the
# desktop build runs cleanly on Windows runners (no -W flags on MSVC) and on
# macOS/Linux (no /W flags on Clang/GCC).
if(MSVC)
    # /W4 /WX matches the GCC/Clang `-Wall -Wextra -Werror` discipline. A few
    # MSVC-only warnings have no GCC equivalent at -Wall -Wextra and are
    # suppressed below rather than fought at every call site:
    #   C4702 — "unreachable code" inside `if constexpr` early-return branches
    #           (a well-known MSVC quirk: it analyses the discarded branch as
    #           live and flags everything after `return;`). GCC understands the
    #           constexpr discard and doesn't warn.
    #   C4244 / C4267 — implicit narrowing (int→smaller, size_t→smaller). GCC
    #           only warns under -Wconversion, which is not part of -Wall
    #           -Wextra. The codebase uses explicit static_cast at the sites
    #           where loss-of-data is the real concern; suppressing here brings
    #           MSVC noise level back in line with GCC.
    #   C5285 — vendored doctest.h (v2.4.11) forward-declares `std::tuple` as a
    #           libstdc++/libc++ portability workaround; the VS 2026 MSVC STL
    #           newly flags that as "specializing a standard template". It's in
    #           third-party header code we don't own and GCC/Clang never warn.
    add_compile_options(/W4 /WX /wd4702 /wd4244 /wd4267 /wd5285)
    # MSVC marks the standard C library (strcpy, sprintf, fopen, localtime, …)
    # as deprecated and emits C4996; /WX makes that fatal. Silence the family —
    # the code uses these standard functions deliberately and consistently with
    # the POSIX side, and the "secure" `_s` variants are a Microsoft extension.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
                            _WINSOCK_DEPRECATED_NO_WARNINGS)
    # Static MSVC runtime so the Windows binary doesn't need vcredist.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

# `uv` is the project's Python launcher (see CLAUDE.md / scripts/MoonDeck.md).
# The build invokes Python helpers for build_info.h generation and UI embedding;
# resolving them through `uv run python …` keeps Windows (where `python3` isn't
# on PATH) on the same path as macOS / Linux without a `find_package(Python3)`
# detour and without per-host scaffolding.
find_program(UV_EXECUTABLE NAMES uv REQUIRED
             HINTS "$ENV{USERPROFILE}/.local/bin" "$ENV{HOME}/.local/bin")

# Core library. Most modules ship header-only (single .h file with implementation
# inline). Core service modules that bridge to the platform (HTTP server,
# filesystem, network, …) ship as .h + .cpp per CLAUDE.md — list those .cpps
# here so both the application and the test binary link them.
add_library(mm_core STATIC
    src/core/Control.cpp
    src/core/HttpServerModule.cpp
    src/core/FilesystemModule.cpp
    src/core/Scheduler.cpp
    src/core/moonlive/MoonLive.cpp
    src/core/moonlive/MoonLiveCompiler.cpp
)
target_include_directories(mm_core PUBLIC src/)
target_link_libraries(mm_core PUBLIC mm_platform)
# `add_dependencies(mm_core ui_embed)` is below, after the ui_embed target is defined.

# Platform library (desktop). moonlive_emit.cpp is the desktop MoonLive backend (host-ISA
# codegen) — it lives here because emitted machine code is platform/ISA-specific.
add_library(mm_platform
    src/platform/desktop/platform_desktop.cpp
    src/platform/desktop/moonlive_emit.cpp
)
target_include_directories(mm_platform PUBLIC src/ src/platform/desktop/)
# Winsock for the desktop socket surface on Windows.
target_link_libraries(mm_platform PUBLIC $<$<PLATFORM_ID:Windows>:ws2_32>)

# Generate build_info.h from library.json (carries version, build date, board name).
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/src/core/build_info.h
    COMMAND ${UV_EXECUTABLE} run python ${CMAKE_SOURCE_DIR}/scripts/build/generate_build_info.py
    DEPENDS ${CMAKE_SOURCE_DIR}/library.json ${CMAKE_SOURCE_DIR}/scripts/build/generate_build_info.py
    COMMENT "Generating build_info.h"
)
add_custom_target(build_info_gen DEPENDS ${CMAKE_SOURCE_DIR}/src/core/build_info.h)

# Embed UI files as C arrays. Pass UV_EXECUTABLE through as a single value
# (no semicolons on the command line — those would either get split by `make`
# on POSIX hosts or land as literals in the value on MSBuild) and let
# embed_ui.cmake assemble the `<uv> run python` list internally.
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h
    COMMAND ${CMAKE_COMMAND} -DUI_DIR=${CMAKE_SOURCE_DIR}/src/ui -DOUT=${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h -DUV_EXECUTABLE=${UV_EXECUTABLE} -P ${CMAKE_SOURCE_DIR}/src/ui/embed_ui.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ui/index.html ${CMAKE_SOURCE_DIR}/src/ui/app.js ${CMAKE_SOURCE_DIR}/src/ui/style.css ${CMAKE_SOURCE_DIR}/src/ui/install-picker.js ${CMAKE_SOURCE_DIR}/src/ui/preview3d.js ${CMAKE_SOURCE_DIR}/src/ui/moonlight-logo.png ${CMAKE_SOURCE_DIR}/src/ui/embed_ui.cmake
    COMMENT "Embedding UI files"
)
add_custom_target(ui_embed DEPENDS ${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h)

# mm_core's HttpServerModule.cpp consumes ui_embedded.h and SystemModule.h
# (a mm_core public header) consumes build_info.h. Anything that links mm_core
# transitively includes those headers, so both generated files must exist
# before any mm_core compile unit starts. CI's parallel clean build exposes
# the race (test/ compiles SystemModule.h while build_info_gen is still
# running); wiring the dep here makes the ordering explicit on every consumer.
add_dependencies(mm_core ui_embed build_info_gen)

# Application
add_executable(projectMM src/main.cpp src/platform/desktop/main_desktop.cpp)
target_link_libraries(projectMM PRIVATE mm_core mm_platform)
add_dependencies(projectMM ui_embed build_info_gen)

# Tests
enable_testing()
add_subdirectory(test)
