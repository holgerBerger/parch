#include <catch2/catch_test_macros.hpp>
#include "arch/format.h"
#include <cstring>
#include <arpa/inet.h>

using namespace arch::format;

TEST_CASE("htobe64 roundtrip", "[endian]") {
    auto v = static_cast<std::uint64_t>(0xDEADBEEF12345678);
    auto out = htobe64_safe(v);
    REQUIRE(betoh64_safe(out) == v);
}

TEST_CASE("htobe32 roundtrip", "[endian]") {
    auto v = static_cast<std::uint32_t>(0xCAFEBABE);
    auto out = htobe32_safe(v);
    REQUIRE(betoh32_safe(out) == v);
}

TEST_CASE("htobe16 roundtrip", "[endian]") {
    auto v = static_cast<std::uint16_t>(0xBEEF);
    auto out = htobe16_safe(v);
    REQUIRE(betoh16_safe(out) == v);
}

TEST_CASE("hton preserves zero", "[endian]") {
    REQUIRE(htobe64_safe(0) == 0);
    REQUIRE(htobe32_safe(0) == 0);
    REQUIRE(htobe16_safe(0) == 0);
}
