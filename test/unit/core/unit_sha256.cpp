// @module sha256

#include "doctest.h"
#include "core/sha256.h"

#include <string>

namespace {
std::string hex(const std::string& in) {
    char buf[mm::kSha256DigestSize * 2 + 1];
    mm::sha256Hex(in.data(), in.size(), buf, mm::kSha256DigestSize);
    return std::string(buf);
}
}  // namespace

/// The digest matches the published FIPS 180-4 values, so this is SHA-256 rather than a function
/// that merely agrees with itself.
///
/// These three are the standard vectors: the empty string, the one-block "abc" example from the
/// specification's own appendix, and the two-block example that exercises the padding path where
/// the length does not fit beside the terminator.
TEST_CASE("the digest matches the published SHA-256 test vectors") {
    CHECK(hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
          == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

/// A message that lands exactly on a block boundary, and one just over it, both hash correctly.
///
/// This is where a padding bug hides: 55 bytes leaves room for the length, 56 does not and forces a
/// second block, and 64 is a whole block with the padding entirely in the next one.
TEST_CASE("messages around the block boundary hash correctly") {
    CHECK(hex(std::string(55, 'a'))
          == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(hex(std::string(56, 'a'))
          == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(hex(std::string(64, 'a'))
          == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

/// A longer message spanning several blocks, pinning the loop rather than a single compression.
TEST_CASE("a multi-block message hashes correctly") {
    CHECK(hex(std::string(1000, 'a'))
          == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

/// The hex helper truncates to the requested length and always terminates, which is what the
/// installation id relies on for its 32 characters.
TEST_CASE("the hex helper truncates the digest to the requested length") {
    char buf[64] = {};
    mm::sha256Hex("abc", 3, buf, 16);
    CHECK(std::string(buf).size() == 32);
    CHECK(std::string(buf) == "ba7816bf8f01cfea414140de5dae2223");
}
