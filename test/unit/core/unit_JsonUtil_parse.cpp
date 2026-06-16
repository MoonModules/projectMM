// @module JsonUtil

// Pins the recursive JSON reader (mm::json::parse + the walk accessors). The flat
// helpers (parseString/hasKey/parseInt/parseBool) stay first-match-only and are
// covered elsewhere; this file exercises the recursive-descent parser that fills a
// bounded node arena. The headline case is an array of small objects — the persisted
// device list DevicesModule loads at boot. Robustness is the other half: malformed,
// truncated, garbage, null, and oversized inputs must fail cleanly with no crash.

#include "doctest.h"
#include "core/JsonUtil.h"

#include <cstring>
#include <string>

using namespace mm;

TEST_CASE("parse a flat object reads each typed field") {
    json::JsonDoc doc;
    REQUIRE(json::parse("{\"a\":1,\"b\":\"x\",\"c\":true}", doc));
    const json::JsonNode* root = doc.rootNode();
    REQUIRE(root != nullptr);
    CHECK(root->type == json::JsonType::Object);

    CHECK(json::readInt(json::member(doc, root, "a")) == 1);

    char s[8];
    CHECK(json::readString(json::member(doc, root, "b"), s, sizeof(s)));
    CHECK(std::strcmp(s, "x") == 0);

    CHECK(json::readBool(json::member(doc, root, "c")) == true);

    // Absent key -> nullptr -> safe defaults.
    CHECK(json::member(doc, root, "missing") == nullptr);
    CHECK(json::readInt(json::member(doc, root, "missing"), -1) == -1);
}

TEST_CASE("parse an array of objects (the persisted device list use case)") {
    json::JsonDoc doc;
    const char* devices =
        "[{\"ip\":\"192.168.1.5\",\"name\":\"WLED\",\"type\":2},"
        "{\"ip\":\"192.168.1.9\",\"name\":\"MM-AB\",\"type\":1}]";
    REQUIRE(json::parse(devices, doc));

    const json::JsonNode* arr = doc.rootNode();
    REQUIRE(arr != nullptr);
    CHECK(arr->type == json::JsonType::Array);
    REQUIRE(json::arraySize(doc, arr) == 2);

    const json::JsonNode* d0 = json::element(doc, arr, 0);
    REQUIRE(d0 != nullptr);
    char ip[24];
    CHECK(json::readString(json::member(doc, d0, "ip"), ip, sizeof(ip)));
    CHECK(std::strcmp(ip, "192.168.1.5") == 0);
    char name[16];
    CHECK(json::readString(json::member(doc, d0, "name"), name, sizeof(name)));
    CHECK(std::strcmp(name, "WLED") == 0);
    CHECK(json::readInt(json::member(doc, d0, "type")) == 2);

    const json::JsonNode* d1 = json::element(doc, arr, 1);
    REQUIRE(d1 != nullptr);
    CHECK(json::readString(json::member(doc, d1, "ip"), ip, sizeof(ip)));
    CHECK(std::strcmp(ip, "192.168.1.9") == 0);
    CHECK(json::readString(json::member(doc, d1, "name"), name, sizeof(name)));
    CHECK(std::strcmp(name, "MM-AB") == 0);
    CHECK(json::readInt(json::member(doc, d1, "type")) == 1);

    // Out-of-range element is safe.
    CHECK(json::element(doc, arr, 2) == nullptr);
    CHECK(json::element(doc, arr, -1) == nullptr);
}

TEST_CASE("parse a nested object") {
    json::JsonDoc doc;
    REQUIRE(json::parse("{\"outer\":{\"inner\":{\"v\":42}}}", doc));
    const json::JsonNode* outer = json::member(doc, doc.rootNode(), "outer");
    REQUIRE(outer != nullptr);
    CHECK(outer->type == json::JsonType::Object);
    const json::JsonNode* inner = json::member(doc, outer, "inner");
    REQUIRE(inner != nullptr);
    CHECK(json::readInt(json::member(doc, inner, "v")) == 42);
}

TEST_CASE("escaped quotes and backslashes round-trip inside a string value") {
    json::JsonDoc doc;
    // Source bytes: {"s":"a\"b\\c"} -> value is  a"b\c
    REQUIRE(json::parse("{\"s\":\"a\\\"b\\\\c\"}", doc));
    char s[16];
    CHECK(json::readString(json::member(doc, doc.rootNode(), "s"), s, sizeof(s)));
    CHECK(std::strcmp(s, "a\"b\\c") == 0);

    // \n escape decodes to a real newline.
    json::JsonDoc doc2;
    REQUIRE(json::parse("{\"s\":\"line1\\nline2\"}", doc2));
    CHECK(json::readString(json::member(doc2, doc2.rootNode(), "s"), s, sizeof(s)));
    CHECK(std::strcmp(s, "line1\nline2") == 0);
}

TEST_CASE("negative and fractional numbers") {
    json::JsonDoc doc;
    REQUIRE(json::parse("{\"a\":-7,\"b\":3.9}", doc));
    CHECK(json::readInt(json::member(doc, doc.rootNode(), "a")) == -7);
    // Fractional values truncate to int (we never persist floats).
    CHECK(json::readInt(json::member(doc, doc.rootNode(), "b")) == 3);
}

TEST_CASE("malformed inputs fail cleanly without crashing") {
    json::JsonDoc doc;

    CHECK_FALSE(json::parse(nullptr, doc));
    CHECK_FALSE(doc.valid());

    CHECK_FALSE(json::parse("", doc));
    CHECK_FALSE(doc.valid());

    CHECK_FALSE(json::parse("[{\"ip\":", doc));        // truncated
    CHECK_FALSE(doc.valid());

    CHECK_FALSE(json::parse("{\"a\":1", doc));          // unbalanced brace
    CHECK_FALSE(json::parse("}{][", doc));              // garbage
    CHECK_FALSE(json::parse("{\"a\":}", doc));          // missing value
    CHECK_FALSE(json::parse("{\"a\" 1}", doc));         // missing colon
    CHECK_FALSE(json::parse("{\"a\":1}garbage", doc));  // trailing garbage
    CHECK_FALSE(json::parse("\"unterminated", doc));    // unterminated string
    CHECK_FALSE(json::parse("[1,2,", doc));             // trailing comma + truncation

    // After a failed parse the doc stays invalid; accessors on it are safe.
    CHECK_FALSE(doc.valid());
    CHECK(doc.rootNode() == nullptr);
    CHECK(json::member(doc, doc.rootNode(), "x") == nullptr);
    CHECK(json::arraySize(doc, doc.rootNode()) == 0);
}

TEST_CASE("overflow safety: too many nodes fails cleanly") {
    // An array of more elements than kMaxNodes can hold (the array node itself plus one
    // node per element). Build it dynamically; parse must fail, not overrun the arena.
    std::string big = "[";
    for (int i = 0; i < json::kMaxNodes + 50; i++) {
        if (i) big += ",";
        big += "1";
    }
    big += "]";
    REQUIRE(big.size() + 1 <= json::kMaxJsonLen);  // stays inside the text buffer

    json::JsonDoc doc;
    CHECK_FALSE(json::parse(big.c_str(), doc));
    CHECK_FALSE(doc.valid());
}

TEST_CASE("overflow safety: nesting deeper than kMaxDepth fails cleanly") {
    std::string deep;
    for (int i = 0; i < json::kMaxDepth + 5; i++) deep += "[";
    for (int i = 0; i < json::kMaxDepth + 5; i++) deep += "]";

    json::JsonDoc doc;
    CHECK_FALSE(json::parse(deep.c_str(), doc));
    CHECK_FALSE(doc.valid());
}

TEST_CASE("input longer than the text buffer fails cleanly") {
    std::string huge(json::kMaxJsonLen + 100, 'x');
    huge[0] = '"';
    json::JsonDoc doc;
    CHECK_FALSE(json::parse(huge.c_str(), doc));
    CHECK_FALSE(doc.valid());
}
