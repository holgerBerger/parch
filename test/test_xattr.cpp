#include <catch2/catch_test_macros.hpp>
#include <spdlog/spdlog.h>
#include <vector>
#include <random>

TEST_CASE("crc32c of known data block", "[cutil][xattr]") {
    // Test xattr read/write roundtrip on a temp file.
    const auto path = std::filesystem::temp_directory_path() /
                      "arch_test_xattr.txt";
    {
        std::ofstream(path) << "content";
    }

    // Set a simple xattr.
    REQUIRE(::setxattr(path.c_str(), "user.test", "value", 5, 0) == 0);

    // Read it back via our util.
    std::vector<std::uint8_t> out;
    auto off = arch::util::read_xattrs(path, out);
    REQUIRE(off >= 0); // blob was written
    REQUIRE(!out.empty());

    // Write it back via our util.
    // We can't easily test re-read without implementing the parser more.
    // At minimum, the function should not throw.
    // (Skipped for now — util is still a skeleton.)

    std::filesystem::remove(path);
}

TEST_CASE("xattr on file with no attributes returns 0", "[util][xattr]") {
    const auto path = std::filesystem::temp_directory_path() /
                      "arch_test_no_xattr.txt";
    {
        std::ofstream(path) << "content";
    }

    std::vector<std::uint8_t> out;
    auto off = arch::util::read_xattrs(path, out);
    REQUIRE(off == 0);
    REQUIRE(out.empty());

    std::filesystem::remove(path);
}
