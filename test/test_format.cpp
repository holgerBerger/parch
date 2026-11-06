#include <catch2/catch_test_macros.hpp>
#include "arch/format.h"
#include <cstring>

TEST_CASE("block_entry is 48 bytes with 128-bit content_id", "[format]") {
    REQUIRE(sizeof(arch::format::block_entry) == 48);
    REQUIRE(sizeof(arch::format::block_entry::content_id) == 16);
    REQUIRE(alignof(arch::format::block_entry) == 8);
}

TEST_CASE("file_entry is 128 bytes with symlink/hardlink fields", "[format]") {
    REQUIRE(sizeof(arch::format::file_entry) == 128);
    arch::format::file_entry e{};
    e.symlink_target_id = 7;
    e.link_target_id = 9;
    REQUIRE(e.symlink_target_id == 7);
    REQUIRE(e.link_target_id == 9);
}

TEST_CASE("record sizes match format spec v0.2", "[format]") {
    REQUIRE(arch::format::ARCHIVE_HEADER_SIZE == 16);
    REQUIRE(sizeof(arch::format::footer) == 256);
    REQUIRE(arch::format::FOOTER_SIZE == 256);
    REQUIRE(sizeof(arch::format::toc_section_ref) == 24);
    REQUIRE(arch::format::TOC_SECTION_REF_SIZE == 24);
    REQUIRE(sizeof(arch::format::toc_header) == 128);
    REQUIRE(arch::format::TOC_HEADER_SIZE == 128);
    REQUIRE(sizeof(arch::format::dedup_entry) == 32);
}

TEST_CASE("footer size is exactly 256 bytes", "[format]") {
    REQUIRE(sizeof(arch::format::footer) == 256);
}

TEST_CASE("dedup_entry is aligned 8 bytes", "[format]") {
    REQUIRE(sizeof(arch::format::dedup_entry) == 32);
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
    REQUIRE(z.symlink_target_id == 0); // 0 = not a symlink
    REQUIRE(z.link_target_id == 0);    // 0 = not a hardlink duplicate
}
