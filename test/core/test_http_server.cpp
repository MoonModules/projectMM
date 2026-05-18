#include "doctest.h"
#include "core/modules/HttpServerModule.h"
#include "core/AppState.h"
#include "platform/Timing.h"

using namespace mm;

TEST_CASE("HttpServerModule name and controls") {
    HttpServerModule server;
    server.addControls();
    CHECK(std::strcmp(server.name(), "HTTP Server") == 0);
    CHECK(server.controlCount() == 1);
    CHECK(server.control(0)->u16.value == 8080);
}

TEST_CASE("HttpServerModule starts and stops") {
    HttpServerModule server;
    AppState appState;
    server.setAppState(&appState);
    server.setUiPath("src/ui");
    server.addControls();
    server.setControl(0, uint16_t(18081)); // high port to avoid conflicts

    server.setup();
    // loop() should not block when no connections
    server.loop();
    server.loop();
    server.teardown();
    // Should not crash on double teardown
    server.teardown();
}

TEST_CASE("HttpServerModule loop is non-blocking") {
    HttpServerModule server;
    AppState appState;
    server.setAppState(&appState);
    server.addControls();
    server.setControl(0, uint16_t(18082));

    server.setup();

    auto start = mm::platform::millis();
    for (int i = 0; i < 100; ++i) {
        server.loop();
    }
    auto elapsed = mm::platform::millis() - start;

    server.teardown();

    // 100 loops with no connections should take < 100ms
    CHECK(elapsed < 100);
}
