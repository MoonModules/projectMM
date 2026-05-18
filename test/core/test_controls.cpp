#include "doctest.h"
#include "core/Control.h"
#include <cstring>

using namespace mm;

TEST_CASE("Control default construction") {
    Control c;
    CHECK(c.type == ControlType::Uint16);
    CHECK(c.u16.value == 0);
    CHECK(c.name[0] == '\0');
}

TEST_CASE("Control setName") {
    Control c;
    c.setName("brightness");
    CHECK(std::strcmp(c.name, "brightness") == 0);
}

TEST_CASE("Control setName truncates at 23 chars") {
    Control c;
    c.setName("this_name_is_way_too_long_for_the_buffer");
    CHECK(std::strlen(c.name) == 23);
    CHECK(c.name[23] == '\0');
}

TEST_CASE("Control Uint16 fields") {
    Control c;
    c.type = ControlType::Uint16;
    c.u16.value = 100;
    c.u16.min = 0;
    c.u16.max = 255;
    CHECK(c.u16.value == 100);
    CHECK(c.u16.min == 0);
    CHECK(c.u16.max == 255);
}

TEST_CASE("Control Bool fields") {
    Control c;
    c.type = ControlType::Bool;
    c.b.value = true;
    CHECK(c.b.value == true);
    c.b.value = false;
    CHECK(c.b.value == false);
}

TEST_CASE("Control Text fields") {
    Control c;
    c.type = ControlType::Text;
    std::strncpy(c.text.value, "192.168.1.1", sizeof(c.text.value) - 1);
    c.text.value[sizeof(c.text.value) - 1] = '\0';
    CHECK(std::strcmp(c.text.value, "192.168.1.1") == 0);
}

TEST_CASE("Control Text truncates at 63 chars") {
    Control c;
    c.type = ControlType::Text;
    const char* long_text =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    std::strncpy(c.text.value, long_text, sizeof(c.text.value) - 1);
    c.text.value[sizeof(c.text.value) - 1] = '\0';
    CHECK(std::strlen(c.text.value) == 63);
}
