#include <catch2/catch_test_macros.hpp>
#include "arch/format.h"
#include <cstring>

TEST_CASE("block_entry size and alignment", "[format]") {
    REQUIRE(sizeof(arch::format::block_entry) == 32);
    REQUIRE(alignof(arch::format::block_entry) == 8);
}

TEST_CASE("footer size is exactly 256 bytes", "[format]") {
    REQUIRE(sizeof(arch::format::footer) == 256);
}

TEST_CASE("dedup_entry is aligned 8 bytes", "[format]") {
    REQUIRE(sizeof(arch::format::dedup_entry) == 24);
    REQUIRE(alignof(arch::format::dedup_entry) == 8);
}

TEST_CASE("file_entry contains all expected fields", "[format]") {
    arch::format::file_entry e{};
    e.mode            = 0100644;
    e.uid             = 1000;
    e.gid             = 1000;
    e.mtime_sec       = 1700000000;
    e.mtime_nsec      = 123456;
    e.uncompressed_size = 4096;
    e.block_count     = 1;
    e.checksum        = 0xDEADBEEFu;

    // Verify zero-init works
    arch::format::file_entry z{};
    REQUIRE(z.mode == 0);
    REQUIRE(z.uncompressed_size == 0);
    REQUIRE(z.block_count == 0);
}
