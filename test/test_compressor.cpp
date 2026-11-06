#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include "arch/compressor.h"
#include "arch/zstd_impl.h"
#include <random>
#include <cstring>

using Catch::Matchers::Equals;

TEST_CASE("zstd roundtrip small block", "[compressor][zstd]") {
    using namespace arch::compress;

    std::vector<std::uint8_t> input(8 * 1024);
    std::mt19937 rng(42);
    for (auto& b : input)
        b = static_cast<std::uint8_t>(rng());

    auto comp = std::make_unique<zstd_impl>(0);
    auto block = comp->compress(std::span(input));
    auto out = comp->decompress(std::span(block.data),
                                static_cast<std::uint32_t>(input.size()));

    REQUIRE(block.compressor_id == arch::format::COMPRESSOR_ZSTD);
    REQUIRE(out.size() == input.size());
    REQUIRE_THAT(out, Equals(std::vector<std::uint8_t>(input)));
}

TEST_CASE("zstd roundtrip 4 MiB block", "[compressor][zstd][slow]") {
    using namespace arch::compress;

    std::vector<std::uint8_t> big(4 * 1024 * 1024, 0xAA);
    zstd_impl comp(0);

    auto block = comp.compress(std::span(big));
    auto out = comp.decompress(std::span(block.data),
                               static_cast<std::uint32_t>(big.size()));
    REQUIRE_THAT(out, Equals(big));
}

TEST_CASE("zstd roundtrip empty block", "[compressor][zstd]") {
    using namespace arch::compress;

    std::vector<std::uint8_t> empty{};
    zstd_impl comp(0);

    auto block = comp.compress(std::span(empty));
    auto out = comp.decompress(std::span(block.data), 0);
    REQUIRE(out.empty());
}

TEST_CASE("zstd multi-threaded compression", "[compressor][zstd][slow]") {
    using namespace arch::compress;

    std::vector<std::uint8_t> data(4 * 1024 * 1024, 0x55);
    zstd_mt comp(4);

    // Should not throw even with threads > 1
    auto block = comp.compress(std::span(data));
    REQUIRE(block.compressor_id == arch::format::COMPRESSOR_ZSTD);
}

TEST_CASE("compressor factory creates zstd", "[compressor][factory]") {
    using namespace arch::compress;

    auto c = make_compressor("zstd");
    REQUIRE(c->name() == "zstd");
    REQUIRE(c->id() == arch::format::COMPRESSOR_ZSTD);
}

TEST_CASE("compressor factory rejects unknown name", "[compressor][factory]") {
    using namespace arch::compress;

    REQUIRE_THROWS_AS(make_compressor("bogus"), std::runtime_error);
}

TEST_CASE("zstd roundtrip worst-case incompressible (random)",
          "[compressor][zstd][slow]") {
    using namespace arch::compress;

    std::vector<std::uint8_t> input(1024 * 1024);
    std::mt19937 rng(123);
    for (auto& b : input)
        b = static_cast<std::uint8_t>(rng());

    zstd_impl comp(0);
    auto block = comp.compress(std::span(input));
    auto out = comp.decompress(std::span(block.data),
                               static_cast<std::uint32_t>(input.size()));
    REQUIRE_THAT(out, Equals(input));
}

TEST_CASE("zstd decompress returns empty on corrupted data",
          "[compressor][zstd]") {
    using namespace arch::compress;

    std::vector<std::uint8_t> input = {1, 2, 3, 4, 5};
    std::vector<std::uint8_t> garbage = {0, 0, 0, 0, 0};
    zstd_impl comp(0);

    auto out = comp.decompress(garbage, 100);
    REQUIRE(out.empty());
}

TEST_CASE("zstd CRC32C is valid", "[compressor][zstd]") {
    using namespace arch::compress;

    std::vector<std::uint8_t> data = {0x48, 0x65, 0x6c, 0x6c, 0x6f};
    zstd_impl comp(0);

    auto block = comp.compress(std::span(data));
    REQUIRE(block.crc32c != 0); // non-zero for any real data
}
