#include <catch2/catch_test_macros.hpp>
#include "arch/prefetch.h"
#include "arch/io_uring_pool.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <random>

TEST_CASE("prefetcher delivers chunks in order", "[prefetch]") {
    // Write a temp file with known content.
    const char tmpname[] = "/tmp/arch_prefetch_XXXXXX";
    int fd = mkstemp(const_cast<char*>(tmpname));
    REQUIRE(fd >= 0);

    const std::size_t total = 256 * 1024; // 256 KiB
    const std::size_t chunk = 64 * 1024;  // 4 chunks

    // Fill with deterministic data.
    std::vector<std::uint8_t> expected(total);
    std::mt19937 rng(42);
    for (auto& b : expected) b = static_cast<std::uint8_t>(rng());
    ::write(fd, expected.data(), total);
    ::lseek(fd, 0, SEEK_SET);

    arch::io::io_uring_pool pool(512, 512, 2);
    arch::io::prefetcher pf(pool, total, chunk);
    pf.start(fd, total);

    std::uint64_t offset = 0;
    while (true) {
        auto result = pf.pop_ready_chunk();
        if (!result) break;

        REQUIRE(result->first == offset);
        offset += result->second.size();
        REQUIRE(result->second ==
                std::vector<std::uint8_t>(
                    expected.begin() + result->first,
                    expected.begin() + result->first + result->second.size()));
    }

    REQUIRE(offset == total);

    ::close(fd);
    unlink(tmpname);
}

TEST_CASE("prefetcher handles empty file", "[prefetch]") {
    const char tmpname[] = "/tmp/arch_pf_empty_XXXXXX";
    int fd = mkstemp(const_cast<char*>(tmpname));
    REQUIRE(fd >= 0);

    arch::io::io_uring_pool pool(256, 256, 1);
    arch::io::prefetcher pf(pool, 64 * 1024, 64 * 1024);
    pf.start(fd, 0);

    auto result = pf.pop_ready_chunk();
    REQUIRE(!result.has_value());

    ::close(fd);
    unlink(tmpname);
}

TEST_CASE("prefetcher is_done returns true after full consume", "[prefetch]") {
    const char tmpname[] = "/tmp/arch_pf_done_XXXXXX";
    int fd = mkstemp(const_cast<char*>(tmpname));
    REQUIRE(fd >= 0);

    std::string data = "small file content";
    ::write(fd, data.data(), data.size());
    ::lseek(fd, 0, SEEK_SET);

    arch::io::io_uring_pool pool(256, 256, 1);
    arch::io::prefetcher pf(pool, 1024, 512);
    pf.start(fd, data.size());

    // Consume all chunks.
    while (auto r = pf.pop_ready_chunk()) {
        // consumed
    }

    REQUIRE(pf.is_done());

    ::close(fd);
    unlink(tmpname);
}
