#include <catch2/catch_test_macros.hpp>
#include "arch/format.h"
#include <cstring>
#include <arpa/inet.h>

using namespace arch::format;

TEST_CASE("footer all-zeros initial state", "[format][footer]") {
    footer f{};
    REQUIRE(f.magic == std::array<std::uint8_t, 4>{0, 0, 0, 0});
    REQUIRE(f.version == 0);
    REQUIRE(f.toc_count == 0);
    REQUIRE(f.total_uncompressed_size == 0);
}

TEST_CASE("footer roundtrip via memcpy (serialize/deserialize) ",
          "[format][footer]") {
    footer original{};
    std::memset(original.magic.data(), 'A', 4); // placeholder magic
    original.version = 0x0001;
    original.toc_count = 42;
    original.total_uncompressed_size = 1024 * 1024;

    // Serialize to raw bytes.
    std::vector<std::uint8_t> buf(sizeof(footer), 0);
    std::memcpy(buf.data(), &original, sizeof(footer));

    // Deserialize.
    footer copy{};
    std::memcpy(&copy, buf.data(), sizeof(footer));

    // Compare field by field.
    REQUIRE(copy.magic == original.magic);
    REQUIRE(copy.version == original.version);
    REQUIRE(copy.toc_count == original.toc_count);
    REQUIRE(copy.total_uncompressed_size == original.total_uncompressed_size);
}
