#pragma once

// Platform detection — compile-time only, no runtime cost.
// This header lives in src/platform/ and is the ONE place where
// platform #ifdefs are allowed.

#if defined(__APPLE__) || defined(_WIN32) || (defined(__linux__) && !defined(__arm__))
    #define MM_PLATFORM_DESKTOP
#elif defined(ESP_PLATFORM)
    #define MM_PLATFORM_ESP32
#elif defined(__linux__) && defined(__arm__)
    #define MM_PLATFORM_RPI
#endif
