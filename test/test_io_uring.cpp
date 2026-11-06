#include <catch2/catch_test_macros.hpp>
#include "arch/io_uring_pool.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

TEST_CASE("io_uring_pool write then read", "[io_uring][pool]") {
    const char tmpname[] = "/tmp/arch_test_iouring_XXXXXX";
    int tfd = mkstemp(const_cast<char*>(tmpname));
    REQUIRE(tfd >= 0);

    arch::io::io_uring_pool pool(256, 256, 2);

    std::string hello = "Hello, io_uring!";
    std::vector<std::uint8_t> buf(hello.size(), 0);
    volatile bool write_done = false;
    volatile bool read_done = false;

    pool.submit_write(tfd, hello.data(), hello.size(), 0, nullptr,
                      [&write_done, &hello](int, void*, int res) {
        REQUIRE(res == static_cast<int>(hello.size()));
        write_done = true;
    });

    pool.submit_read(tfd, buf.data(), buf.size(), 0, nullptr,
                     [&read_done](int, void*, int res) {
        REQUIRE(res >= 0);
        read_done = true;
    });

    pool.drain_and_stop();
    REQUIRE(write_done);
    REQUIRE(read_done);
    REQUIRE(buf ==
            std::vector<std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(hello.data()),
                reinterpret_cast<const std::uint8_t*>(hello.data()) +
                    hello.size()));

    unlink(tmpname);
    ::close(tfd);
}

TEST_CASE("io_uring_pool fsync completes", "[io_uring][pool]") {
    const char tmpname[] = "/tmp/arch_test_fsync_XXXXXX";
    int tfd = mkstemp(const_cast<char*>(tmpname));
    REQUIRE(tfd >= 0);

    arch::io::io_uring_pool pool(256, 256, 1);
    volatile bool done = false;

    pool.submit_fsync(tfd, nullptr,
                      [&done](int, void*, int res) {
        REQUIRE(res >= 0);
        done = true;
    });

    pool.drain_and_stop();
    REQUIRE(done);

    unlink(tmpname);
    ::close(tfd);
}

TEST_CASE("io_uring_pool multiple sequential writes", "[io_uring][pool]") {
    const char tmpname[] = "/tmp/arch_test_multi_XXXXXX";
    int tfd = mkstemp(const_cast<char*>(tmpname));
    REQUIRE(tfd >= 0);

    arch::io::io_uring_pool pool(256, 256, 2);
    std::atomic<int> written{0};

    // Buffers must outstand the async writes: keep them alive until after
    // drain_and_stop() instead of using a per-iteration temporary.
    std::vector<std::string> datas;
    for (int i = 0; i < 8; ++i) {
        datas.push_back("chunk-" + std::to_string(i) + "-padding1234");
    }
    for (int i = 0; i < 8; ++i) {
        uint64_t off = static_cast<uint64_t>(i) * 1024;
        pool.submit_write(tfd, datas[i].data(), datas[i].size(), off, nullptr,
                          [&written](int, void*, int res) {
            REQUIRE(res >= 0);
            written.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool.drain_and_stop();
    REQUIRE(written.load() == 8);

    unlink(tmpname);
    ::close(tfd);
}
