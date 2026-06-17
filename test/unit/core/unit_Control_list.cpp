// @module Control

// Pins the ControlType::List serialization contract (the generic list control that
// backs DevicesModule's discovered-devices view). A List holds no row data itself —
// a ListSource the owning module implements produces rows on demand from the
// module's own storage. These tests verify:
//   - the value serializes as a JSON array of summary objects (one per row),
//   - the metadata carries a parallel `detail` array,
//   - an empty source emits "[]" (robustness: a list with nothing found),
//   - a List is read-only from the browser but PERSISTABLE: the saved array is
//     parsed back on boot via ListSource::restoreList (the recursive mm::json
//     reader's forEachListElement), seeding the cached list before the first scan.

#include "doctest.h"
#include "core/Control.h"
#include "core/JsonSink.h"
#include "core/JsonUtil.h"

#include <cstdint>
#include <cstring>

namespace {

// A tiny fixed-data ListSource standing in for a real module (e.g. DevicesModule).
// Two rows; row 0 is the "self" device. Summary is a compact object; detail adds
// a field, exercising the separate summary/detail paths.
struct StubDevices : mm::ListSource {
    uint8_t n = 2;
    uint8_t listRowCount() const override { return n; }
    void writeListRow(mm::JsonSink& s, uint8_t row) const override {
        if (row == 0) s.append("{\"name\":\"self\",\"ip\":\"192.168.1.10\",\"self\":true}");
        else          s.append("{\"name\":\"WLED-1\",\"ip\":\"192.168.1.50\"}");
    }
    void writeListRowDetail(mm::JsonSink& s, uint8_t row) const override {
        if (row == 0) s.append("{\"name\":\"self\",\"ip\":\"192.168.1.10\",\"type\":\"projectMM\",\"self\":true}");
        else          s.append("{\"name\":\"WLED-1\",\"ip\":\"192.168.1.50\",\"type\":\"WLED\"}");
    }
    // Restore: parse the persisted array with the recursive reader; record the count
    // and the first row's name so a test can prove the round-trip took.
    int restoredCount = -1;
    char firstName[24] = {};
    bool restoreList(const char* json, const char* key) override {
        mm::json::JsonDoc doc;
        if (!mm::json::parse(json, doc)) return false;
        const mm::json::JsonNode* arr = mm::json::member(doc, doc.rootNode(), key);
        if (!arr || arr->type != mm::json::JsonType::Array) return false;
        restoredCount = mm::json::arraySize(doc, arr);
        const mm::json::JsonNode* first = mm::json::element(doc, arr, 0);
        mm::json::readString(mm::json::member(doc, first, "name"), firstName, sizeof(firstName));
        return true;
    }
};

}  // namespace

TEST_CASE("ControlType::List value serializes as an array of row summaries") {
    StubDevices src;
    mm::ControlList controls;
    controls.addList("devices", src);
    REQUIRE(controls.count() == 1);
    CHECK(controls[0].type == mm::ControlType::List);

    mm::JsonSink sink;  // buffer mode
    mm::writeControlValue(sink, controls[0]);
    CHECK(std::strcmp(sink.data(),
        "[{\"name\":\"self\",\"ip\":\"192.168.1.10\",\"self\":true},"
        "{\"name\":\"WLED-1\",\"ip\":\"192.168.1.50\"}]") == 0);
}

TEST_CASE("ControlType::List metadata carries a parallel detail array") {
    StubDevices src;
    mm::ControlList controls;
    controls.addList("devices", src);

    mm::JsonSink sink;
    mm::writeControlMetadata(sink, controls[0]);
    CHECK(std::strcmp(sink.data(),
        ",\"detail\":["
        "{\"name\":\"self\",\"ip\":\"192.168.1.10\",\"type\":\"projectMM\",\"self\":true},"
        "{\"name\":\"WLED-1\",\"ip\":\"192.168.1.50\",\"type\":\"WLED\"}]") == 0);
}

TEST_CASE("ControlType::List with an empty source emits []") {
    StubDevices src;
    src.n = 0;  // nothing discovered
    mm::ControlList controls;
    controls.addList("devices", src);

    mm::JsonSink sink;
    mm::writeControlValue(sink, controls[0]);
    CHECK(std::strcmp(sink.data(), "[]") == 0);
}

TEST_CASE("ControlType::List type identity + persistable + restore round-trip") {
    CHECK(std::strcmp(mm::controlTypeName(mm::ControlType::List), "list") == 0);
    // Persistable: the List value is a JSON array the recursive reader round-trips,
    // restored via ListSource::restoreList (the model owns its (de)serialization).
    CHECK(mm::isPersistable(mm::ControlType::List));
    // applyControlValue on a List drives restoreList (the persistence-overlay load
    // path) and returns Ok — handing the source the saved array to rebuild itself.
    StubDevices src;
    mm::ControlList controls;
    controls.addList("devices", src);
    const char* saved =
        "{\"devices\":[{\"name\":\"WLED-1\",\"ip\":\"192.168.1.50\",\"type\":\"WLED\"},"
        "{\"name\":\"MM-AB\",\"ip\":\"192.168.1.9\",\"type\":\"projectMM\"}]}";
    auto r = mm::applyControlValue(controls[0], saved, "devices", mm::ApplyPolicy::Clamp);
    CHECK(r == mm::ApplyResult::Ok);
    CHECK(src.restoredCount == 2);                       // parsed both rows
    CHECK(std::strcmp(src.firstName, "WLED-1") == 0);    // read a field back
}
