#include "core/types.h"

#include <csignal>
#include <cstdio>

extern void mm_main(volatile bool& keepRunning, mm::lengthType gridW, mm::lengthType gridH);

static volatile bool running = true;

static void signalHandler(int) {
    running = false;
}

int main() {
    std::signal(SIGINT, signalHandler);
    std::printf("Press Ctrl-C to stop.\n");
    mm_main(running, mm::defaultGridSize, mm::defaultGridSize);
    return 0;
}
