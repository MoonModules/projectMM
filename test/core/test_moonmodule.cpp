#include "doctest.h"
#include "core/MoonModule.h"
#include <cstring>

using namespace mm;

// Concrete test module to verify lifecycle and onChange
class TestModule : public MoonModule {
public:
    const char* name() const override { return "test"; }

    int setupCount = 0;
    int loopCount = 0;
    int teardownCount = 0;
    int lastChangedIndex = -1;
    int changeCount = 0;

    void setup() override { ++setupCount; }
    void loop() override { ++loopCount; }
    void teardown() override { ++teardownCount; }
    void onChange(uint8_t index) override {
        lastChangedIndex = index;
        ++changeCount;
    }
};

TEST_CASE("MoonModule lifecycle") {
    TestModule m;
    CHECK(m.setupCount == 0);
    CHECK(m.loopCount == 0);
    CHECK(m.teardownCount == 0);

    m.setup();
    CHECK(m.setupCount == 1);

    m.loop();
    m.loop();
    m.loop();
    CHECK(m.loopCount == 3);

    m.teardown();
    CHECK(m.teardownCount == 1);
}

TEST_CASE("MoonModule name") {
    TestModule m;
    CHECK(std::strcmp(m.name(), "test") == 0);
}

TEST_CASE("MoonModule addControl Uint16") {
    TestModule m;
    uint8_t idx = m.addControl("speed", uint16_t(100), uint16_t(0), uint16_t(255));
    CHECK(idx == 0);
    CHECK(m.controlCount() == 1);

    auto* c = m.control(idx);
    REQUIRE(c != nullptr);
    CHECK(c->type == ControlType::Uint16);
    CHECK(c->u16.value == 100);
    CHECK(c->u16.min == 0);
    CHECK(c->u16.max == 255);
    CHECK(std::strcmp(c->name, "speed") == 0);
}

TEST_CASE("MoonModule addControl Bool") {
    TestModule m;
    uint8_t idx = m.addControl("enabled", true);
    CHECK(idx == 0);
    CHECK(m.controlCount() == 1);

    auto* c = m.control(idx);
    REQUIRE(c != nullptr);
    CHECK(c->type == ControlType::Bool);
    CHECK(c->b.value == true);
}

TEST_CASE("MoonModule addControl Text") {
    TestModule m;
    uint8_t idx = m.addControl("ip", "192.168.1.1");
    CHECK(idx == 0);

    auto* c = m.control(idx);
    REQUIRE(c != nullptr);
    CHECK(c->type == ControlType::Text);
    CHECK(std::strcmp(c->text.value, "192.168.1.1") == 0);
}

TEST_CASE("MoonModule multiple controls") {
    TestModule m;
    uint8_t a = m.addControl("speed", uint16_t(50), uint16_t(0), uint16_t(100));
    uint8_t b = m.addControl("enabled", false);
    uint8_t c = m.addControl("host", "10.0.0.1");
    CHECK(a == 0);
    CHECK(b == 1);
    CHECK(c == 2);
    CHECK(m.controlCount() == 3);
}

TEST_CASE("MoonModule controlByName") {
    TestModule m;
    m.addControl("alpha", uint16_t(10), uint16_t(0), uint16_t(100));
    m.addControl("beta", true);

    auto* found = m.controlByName("beta");
    REQUIRE(found != nullptr);
    CHECK(found->type == ControlType::Bool);
    CHECK(found->b.value == true);

    CHECK(m.controlByName("missing") == nullptr);
}

TEST_CASE("MoonModule control out of bounds returns null") {
    TestModule m;
    CHECK(m.control(0) == nullptr);
    CHECK(m.control(255) == nullptr);
}

TEST_CASE("MoonModule setControl Uint16 triggers onChange") {
    TestModule m;
    uint8_t idx = m.addControl("val", uint16_t(50), uint16_t(0), uint16_t(100));

    m.setControl(idx, uint16_t(75));
    CHECK(m.control(idx)->u16.value == 75);
    CHECK(m.changeCount == 1);
    CHECK(m.lastChangedIndex == 0);
}

TEST_CASE("MoonModule setControl Uint16 clamps to range") {
    TestModule m;
    uint8_t idx = m.addControl("val", uint16_t(50), uint16_t(10), uint16_t(90));

    m.setControl(idx, uint16_t(200));
    CHECK(m.control(idx)->u16.value == 90);

    m.setControl(idx, uint16_t(0));
    CHECK(m.control(idx)->u16.value == 10);
}

TEST_CASE("MoonModule setControl does not call onChange if unchanged") {
    TestModule m;
    uint8_t idx = m.addControl("val", uint16_t(50), uint16_t(0), uint16_t(100));

    m.setControl(idx, uint16_t(50));
    CHECK(m.changeCount == 0);
}

TEST_CASE("MoonModule setControl Bool triggers onChange") {
    TestModule m;
    uint8_t idx = m.addControl("flag", false);

    m.setControl(idx, true);
    CHECK(m.control(idx)->b.value == true);
    CHECK(m.changeCount == 1);

    m.setControl(idx, true);
    CHECK(m.changeCount == 1); // unchanged, no call
}

TEST_CASE("MoonModule setControl Text triggers onChange") {
    TestModule m;
    uint8_t idx = m.addControl("host", "a.com");

    m.setControl(idx, "b.com");
    CHECK(std::strcmp(m.control(idx)->text.value, "b.com") == 0);
    CHECK(m.changeCount == 1);

    m.setControl(idx, "b.com");
    CHECK(m.changeCount == 1); // unchanged
}

TEST_CASE("MoonModule MAX_CONTROLS overflow") {
    TestModule m;
    for (int i = 0; i < MoonModule::MAX_CONTROLS; ++i) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "ctrl%d", i);
        CHECK(m.addControl(buf, uint16_t(0), uint16_t(0), uint16_t(100)) == i);
    }
    CHECK(m.controlCount() == MoonModule::MAX_CONTROLS);
    CHECK(m.addControl("overflow", uint16_t(0), uint16_t(0), uint16_t(100)) == MoonModule::MAX_CONTROLS);
    CHECK(m.controlCount() == MoonModule::MAX_CONTROLS);
}
