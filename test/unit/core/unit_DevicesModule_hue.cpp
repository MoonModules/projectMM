// @module DevicesModule

// Pins the Hue-bridge listing: a HueDriver registers a bridge through upsertHueBridge() (the
// explicit, out-of-band entry — a bridge isn't a UDP-presence device), and DevicesModule lists
// it like any peer, carrying its colour-light count for layout sizing. Also pins the round
// trip through writeListRow / restoreList so a persisted bridge comes back as a Hue row.

#include "doctest.h"
#include "core/DevicesModule.h"
#include "core/JsonSink.h"

#include <cstring>

TEST_CASE("DevicesModule: a Hue bridge is listed with its colour count") {
    mm::DevicesModule dev;
    const uint8_t ip[4] = {192, 168, 1, 143};
    dev.upsertHueBridge(ip, "Hue Ewoud", 7);

    REQUIRE(dev.listRowCount() == 1);
    mm::JsonSink sink;
    dev.writeListRow(sink, 0);
    const char* row = sink.data();
    CHECK(std::strstr(row, "\"name\":\"Hue Ewoud\"") != nullptr);
    CHECK(std::strstr(row, "\"type\":\"Hue bridge\"") != nullptr);
    CHECK(std::strstr(row, "\"colour\":7") != nullptr);
}

TEST_CASE("DevicesModule: upsertHueBridge is idempotent, updates count in place") {
    mm::DevicesModule dev;
    const uint8_t ip[4] = {192, 168, 1, 143};
    dev.upsertHueBridge(ip, "Hue Ewoud", 7);
    dev.upsertHueBridge(ip, "Hue Ewoud", 9);   // same bridge, count changed

    REQUIRE(dev.listRowCount() == 1);           // not a second row
    mm::JsonSink sink;
    dev.writeListRow(sink, 0);
    CHECK(std::strstr(sink.data(), "\"colour\":9") != nullptr);
}

TEST_CASE("DevicesModule: a persisted Hue bridge restores as a Hue row with its count") {
    mm::DevicesModule dev;
    // The shape writeListRow emits (the same JSON the List control persists).
    const char* saved =
        "{\"devices\":[{\"name\":\"Hue Ewoud\",\"ip\":\"192.168.1.143\",\"type\":\"Hue bridge\",\"colour\":7}]}";
    REQUIRE(dev.restoreList(saved, "devices"));
    REQUIRE(dev.listRowCount() == 1);
    mm::JsonSink sink;
    dev.writeListRow(sink, 0);
    CHECK(std::strstr(sink.data(), "\"type\":\"Hue bridge\"") != nullptr);
    CHECK(std::strstr(sink.data(), "\"colour\":7") != nullptr);
}
