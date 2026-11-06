#include <catch2/catch_test_macros.hpp>
#include <crc32c/crc32c.h>
#include <xxhash.h>
#include <cstring>
#include <vector>

TEST_CASE("CRC32C of empty data is 0", "[crc32c]") {
    REQUIRE(crc32c::Extend(0, nullptr, 0) == 0);
}

TEST_CASE("CRC32C of known data", "[crc32c]") {
    // Standard Castagnoli check value: CRC32C("123456789") == 0xE3069283.
    const char* d = "123456789";
    auto c = crc32c::Crc32c(reinterpret_cast<const std::uint8_t*>(d), 9);
    REQUIRE(c == 0xE3069283u);
}

TEST_CASE("CRC32C incremental update", "[crc32c]") {
    std::uint32_t c1 = 0;
    c1 = crc32c::Extend(c1, reinterpret_cast<const std::uint8_t*>("Hello"), 5);
    c1 = crc32c::Extend(c1, reinterpret_cast<const std::uint8_t*>(", Wor"), 5);
    c1 = crc32c::Extend(c1, reinterpret_cast<const std::uint8_t*>("ld!"), 3);

    std::uint32_t c2 = 0;
    c2 = crc32c::Extend(c2, reinterpret_cast<const std::uint8_t*>("Hello, World!"), 13);

    REQUIRE(c1 == c2);
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
