// TOC serialization roundtrip tests — placeholder until Phase 2 lands the
// actual serializer (PLAN.md). Checks the in-memory TOC struct invariants for
// now.
#include <catch2/catch_test_macros.hpp>

#include "arch/format.h"

#include <type_traits>

TEST_CASE("toc structs are trivially copyable", "[format][toc]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<arch::format::toc_header>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<arch::format::file_entry>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<arch::format::block_entry>);
}

TEST_CASE("toc_header default-initialises to zeros", "[format][toc]") {
    arch::format::toc_header hdr{};
    CHECK(hdr.magic == 0u);
    CHECK(hdr.entry_count == 0);
    CHECK(hdr.metadata_crc == 0);
    CHECK(hdr.compression_level == 0);
}

TEST_CASE("toc_header carries section codec + level (v0.2)", "[format][toc]") {
    arch::format::toc_header hdr{};
    hdr.compressor_id = arch::format::COMPRESSOR_ZSTD;
    hdr.compression_level = 5;
    hdr.section_flags = arch::format::SECTION_FLAG_DEDUP;
    CHECK(hdr.compressor_id == 0x01);
    CHECK(hdr.compression_level == 5);
    CHECK((hdr.section_flags & arch::format::SECTION_FLAG_DEDUP) != 0);
}
