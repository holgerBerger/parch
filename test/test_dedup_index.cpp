#include <catch2/catch_test_macros.hpp>
#include "arch/format.h"
#include <algorithm>
#include <vector>

using namespace arch::format;

TEST_CASE("dedup index stores and retrieves hash entries", "[dedup]") {
    std::vector<dedup_entry> idx;

    idx.push_back({0x1000, 4096, 1, 0});
    idx.push_back({0x2000, 8192, 2, 0});
    idx.push_back({0x3000, 12288, 1, 0});

    // Sort by hash.
    std::sort(idx.begin(), idx.end(),
              [](auto& a, auto& b) { return a.content_hash < b.content_hash; });

    // Binary search for 0x2000.
    auto* target = std::lower_bound(
        idx.begin(), idx.end(), static_cast<std::uint64_t>(0x2000),
        [](auto& entry, std::uint64_t hash) {
            return entry.content_hash < hash;
        });
    REQUIRE(target != idx.end());
    REQUIRE(target->content_hash == 0x2000);
    REQUIRE(target->physical_offset == 8192);
    REQUIRE(target->reference_count == 2);
}

TEST_CASE("dedup index handles empty case", "[dedup]") {
    std::vector<dedup_entry> idx;

    auto* target = std::lower_bound(
        idx.begin(), idx.end(), static_cast<std::uint64_t>(0x1234),
        [](auto& entry, std::uint64_t hash) {
            return entry.content_hash < hash;
        });
    REQUIRE(target == idx.end());
}

TEST_CASE("dedup entry size is 24 bytes", "[dedup]") {
    REQUIRE(sizeof(dedup_entry) == 24);
}
