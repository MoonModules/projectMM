#include "core/Scheduler.h"
#include "light/GridLayout.h"
#include "light/RainbowEffect.h"
#include "light/ArtNetSendDriver.h"

#include <csignal>
#include <cstdio>

static volatile bool running = true;

static void signalHandler(int) {
    running = false;
}

int main() {
    std::signal(SIGINT, signalHandler);

    mm::Scheduler scheduler;

    // Layout
    mm::LayoutGroup layoutGroup;
    layoutGroup.setName("LayoutGroup");

    mm::GridLayout grid;
    grid.setName("Grid");
    layoutGroup.addLayout(&grid);

    // Layer + Effect
    mm::Layer layer;
    layer.setName("Layer");
    layer.setLayoutGroup(&layoutGroup);
    layer.setChannelsPerLight(3);

    mm::RainbowEffect rainbow;
    rainbow.setName("Rainbow");
    layer.addEffect(&rainbow);

    // Driver Group + ArtNet
    mm::DriverGroup driverGroup;
    driverGroup.setName("DriverGroup");
    driverGroup.setLayer(&layer);

    mm::ArtNetSendDriver artnet;
    artnet.setName("ArtNet");
    driverGroup.addDriver(&artnet);

    // Register all with scheduler
    scheduler.addModule(&layoutGroup);
    scheduler.addModule(&grid);
    scheduler.addModule(&layer);
    scheduler.addModule(&rainbow);
    scheduler.addModule(&driverGroup);
    scheduler.addModule(&artnet);

    scheduler.setup();

    std::printf("mmv3 running — sending ArtNet to %s (grid %dx%d, %u lights)\n",
                artnet.ip, grid.width, grid.height, layoutGroup.totalLightCount());
    std::printf("Press Ctrl-C to stop.\n");
    std::fflush(stdout);

    while (running) {
        scheduler.tick();
    }

    std::printf("\nShutting down.\n");
    scheduler.teardown();
    return 0;
}
