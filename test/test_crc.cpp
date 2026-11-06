#include <catch2/catch_test_macros.hpp>
#include <crc32c/crc32c.h>
#include <xxhash.h>
#include <cstring>
#include <vector>

TEST_CASE("CRC32C of empty data is 0", "[crc32c]") {
    crc32c::Crc32c c;
    REQUIRE(static_cast<uint32_t>(c.Value()) == 0);
}

TEST_CASE("CRC32C of known data", "[crc32c]") {
    crc32c::Crc32c c;
    c.Update(crc32c::StringView("Hello, CRC32C!", 14));
    // Castagnoli CRC32C of "Hello, CRC32C!" is 0x87413555.
    REQUIRE(c.Value() == 0x87413555u);
}

TEST_CASE("CRC32C incremental update", "[crc32c]") {
    crc32c::Crc32c c1;
    c1.Update(crc32c::StringView("Hello", 5));
    c1.Update(crc32c::StringView(", Wor", 5));
    c1.Update(crc32c::StringView("ld!", 3));

    crc32c::Crc32c c2;
    c2.Update(crc32c::StringView("Hello, World!", 13));

    REQUIRE(c1.Value() == c2.Value());
}

TEST_CASE("XXH3-64 deterministic", "[xxhash]") {
    std::vector<std::uint8_t> data(4096, 0xAB);
    auto h1 = XXH3_64bits(data.data(), data.size());
    auto h2 = XXH3_64bits(data.data(), data.size());
    REQUIRE(h1 == h2);
    REQUIRE(h1 != 0);
}

TEST_CASE("XXH3-64 different data gives different hash", "[xxhash]") {
    std::vector<std::uint8_t> a(4096, 0xAA);
    std::vector<std::uint8_t> b(4096, 0xBB);
    REQUIRE(XXH3_64bits(a.data(), a.size()) !=
            XXH3_64bits(b.data(), b.size()));
}
