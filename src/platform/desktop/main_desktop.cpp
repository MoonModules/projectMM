#include <csignal>
#include <cstdint>   // mm_main() takes uint16_t; <unistd.h> used to pull this in on POSIX
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>

#ifdef _WIN32
#include <io.h>      // _write
#else
#include <unistd.h>
#endif

extern void mm_main(volatile bool& keepRunning, uint16_t httpPort);

static volatile bool running = true;
static bool cleanExit = false;

// Write s to stderr without stdio — safe inside a signal handler.
static void safeWrite(const char* s) {
    size_t len = std::strlen(s);
    while (len > 0) {
#ifdef _WIN32
        int n = ::_write(2 /* stderr fd */, s, static_cast<unsigned int>(len));
#else
        ssize_t n = ::write(STDERR_FILENO, s, len);
#endif
        if (n <= 0) break;
        s += n; len -= static_cast<size_t>(n);
    }
}

static void crashHandler(int sig) {
    const char* name = sig == SIGSEGV ? "SIGSEGV"
                     : sig == SIGABRT ? "SIGABRT"
                     : sig == SIGFPE  ? "SIGFPE"
#ifdef SIGBUS
                     : sig == SIGBUS  ? "SIGBUS"
#endif
                                      : "SIGNAL";
    safeWrite("\n*** CRASH: ");
    safeWrite(name);
    safeWrite(" ***\n");
    // SA_RESETHAND (POSIX) or signal() one-shot semantics (Windows) already
    // restored SIG_DFL; re-raise for OS coredump.
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

#ifdef _WIN32
    // Winsock is initialized by a static RAII guard in platform_desktop.cpp,
    // so it covers test binaries too (they link mm_platform but have their own
    // main()). Nothing to do here.

    // Windows has no sigaction — use signal() with one-shot semantics (the
    // handler is restored to SIG_DFL on entry, so re-raise produces the OS
    // crash dialog). No SIGPIPE on Windows; broken socket writes return an
    // error from send(), which the code already checks.
    signal(SIGSEGV, crashHandler);
    signal(SIGFPE,  crashHandler);
    signal(SIGABRT, crashHandler);
    signal(SIGINT,  [](int) { running = false; });
#else
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
#endif

    std::atexit(atExitHandler);

    std::time_t t = std::time(nullptr);
    char tbuf[32];
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    std::printf("projectMM started at %s\n", tbuf);
    std::printf("Press Ctrl-C to stop.\n");

    mm_main(running, 8080);

    cleanExit = true;
    t = std::time(nullptr);
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    std::printf("projectMM exited cleanly at %s\n", tbuf);
    return 0;
}
