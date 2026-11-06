#include <catch2/catch_test_macros.hpp>
#include "arch/archive.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <thread>

TEST_CASE("writer creates archive with header", "[writer]") {
    const auto path = std::filesystem::temp_directory_path() /
                      "arch_test_writer_empty.arc";

    arch::writer w(path);
    w.finalize();

    // File should exist.
    REQUIRE(std::filesystem::exists(path));

    // Read back first 16 bytes and check magic.
    int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    std::array<std::uint8_t, 4> magic{};
    REQUIRE(::read(fd, magic.data(), 4) == 4);
    REQUIRE(magic[0] == 'A');
    REQUIRE(magic[1] == 'R');
    REQUIRE(magic[2] == 'C');
    REQUIRE(magic[3] == 'H');
    ::close(fd);

    std::filesystem::remove(path);
}

TEST_CASE("writer adds in-memory file", "[writer]") {
    const auto path = std::filesystem::temp_directory_path() /
                      "arch_test_memfile.arc";

    arch::writer w(path);
    w.add_file("hello.txt", std::string_view("Hello, World!"));
    w.finalize();

    // Archive file exists and has more than just the 16-byte header.
    auto sz = std::filesystem::file_size(path);
    REQUIRE(sz > 16);

    std::filesystem::remove(path);
}

TEST_CASE("writer adds real file from filesystem", "[writer][slow]") {
    // Create a real temp file with known content.
    const auto src = std::filesystem::temp_directory_path() / "arch_src_test";
    {
        FILE* f = std::fopen(src.c_str(), "w");
        std::fprintf(f, "test file content for archiver\n");
        std::fclose(f);
    }

    const auto arc = std::filesystem::temp_directory_path() / "arch_write.arc";
    arch::writer w(arc);
    w.add_file(src);
    w.finalize();

    auto sz = std::filesystem::file_size(arc);
    REQUIRE(sz > 16);

    std::filesystem::remove(src);
    std::filesystem::remove(arc);
}

TEST_CASE("writer options default values", "[writer]") {
    arch::writer::options opts;
    REQUIRE(opts.compressor == "zstd");
    REQUIRE(opts.threads > 0);
    REQUIRE(opts.compression_level > 0);
    REQUIRE(opts.dedup == false);
    REQUIRE(opts.pipeline_depth > 0);
    REQUIRE(opts.durable == true);
}

TEST_CASE("writer throws on invalid path", "[writer]") {
    // A path in a non-existent directory should throw.
    const auto path = std::filesystem::temp_directory_path() /
                      "nonexistent_dir_xyz/should_fail.arc";
    REQUIRE_THROWS_AS(arch::writer(path), std::system_error);
}

TEST_CASE("reader throws on missing file", "[reader]") {
    const auto path = std::filesystem::temp_directory_path() /
                      "does_not_exist.arc";
    REQUIRE_THROWS_AS(arch::reader(path), std::system_error);
}
