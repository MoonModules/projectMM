#include "core/types.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <unistd.h>

extern void mm_main(volatile bool& keepRunning, mm::lengthType gridW, mm::lengthType gridH, uint16_t httpPort);

static volatile bool running = true;
static bool cleanExit = false;

// Write s to stderr without stdio — safe inside a signal handler.
static void safeWrite(const char* s) {
    size_t len = std::strlen(s);
    while (len > 0) {
        ssize_t n = ::write(STDERR_FILENO, s, len);
        if (n <= 0) break;
        s += n; len -= static_cast<size_t>(n);
    }
}

static void crashHandler(int sig) {
    const char* name = sig == SIGSEGV ? "SIGSEGV"
                     : sig == SIGABRT ? "SIGABRT"
                     : sig == SIGFPE  ? "SIGFPE"
                     : sig == SIGBUS  ? "SIGBUS"  : "SIGNAL";
    safeWrite("\n*** CRASH: ");
    safeWrite(name);
    safeWrite(" ***\n");
    // SA_RESETHAND already restored SIG_DFL; re-raise for OS coredump.
    raise(sig);
}

// Fires on std::exit() — distinguishes reboot (platform::reboot prints its own
// line first) from a genuine unexpected exit with no preceding crash signal.
static void atExitHandler() {
    if (!cleanExit) {
        std::time_t t = std::time(nullptr);
        char tbuf[32];
        std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
        std::fprintf(stderr, "*** process exited without clean shutdown at %s ***\n", tbuf);
        std::fflush(stderr);
    }
}

int main() {
    // Unbuffer so every line lands in projectMM.log before a crash.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    struct sigaction sa{};
    sa.sa_handler = crashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);

    struct sigaction saInt{};
    saInt.sa_handler = [](int) { running = false; };
    sigemptyset(&saInt.sa_mask);
    sigaction(SIGINT, &saInt, nullptr);

    // Ignore SIGPIPE — write() on a closed TCP connection delivers it by default,
    // which kills the process silently (no crash report) when a browser tab closes
    // mid-response. We handle broken writes via return-value checks instead.
    signal(SIGPIPE, SIG_IGN);

    std::atexit(atExitHandler);

    std::time_t t = std::time(nullptr);
    char tbuf[32];
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    std::printf("projectMM started at %s\n", tbuf);
    std::printf("Press Ctrl-C to stop.\n");

    mm_main(running, mm::defaultGridSize, mm::defaultGridSize, 8080);

    cleanExit = true;
    t = std::time(nullptr);
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    std::printf("projectMM exited cleanly at %s\n", tbuf);
    return 0;
}
