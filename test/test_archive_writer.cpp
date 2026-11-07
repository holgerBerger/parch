#include <catch2/catch_test_macros.hpp>
#include "arch/archive.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <fstream>
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

namespace {
// Small deterministic tree: N small files + 3 copies of one 4 MiB block
// (so --dedup has something to hit).
std::filesystem::path make_profile_tree(const std::string& name) {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    fs::create_directories(root / "sub");
    std::vector<std::uint8_t> dup(4u << 20, 0x5A);
    for (auto& b : dup) b = static_cast<std::uint8_t>(b ^ 0x01); // nonzero (tmpfs)
    for (int i = 0; i < 12; ++i) {
        std::vector<std::uint8_t> data(3000 + i * 97,
                                       static_cast<std::uint8_t>(i + 1));
        auto p = root / ("f" + std::to_string(i));
        FILE* f = std::fopen(p.c_str(), "wb");
        REQUIRE(f);
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }
    for (int i = 0; i < 3; ++i) {
        auto p = root / "sub" / ("dup" + std::to_string(i));
        FILE* f = std::fopen(p.c_str(), "wb");
        REQUIRE(f);
        std::fwrite(dup.data(), 1, dup.size(), f);
        std::fclose(f);
    }
    return root;
}

std::vector<std::uint8_t> slurp(const std::filesystem::path& p) {
    std::vector<std::uint8_t> out(std::filesystem::file_size(p));
    int fd = ::open(p.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    std::size_t off = 0;
    while (off < out.size()) {
        auto n = ::pread(fd, out.data() + off, out.size() - off, off);
        REQUIRE(n > 0);
        off += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return out;
}

arch::writer::options profile_opts(bool profile, int threads) {
    arch::writer::options o;
    o.profile = profile;
    o.threads = threads;
    o.dedup = true;
    return o;
}
} // namespace

TEST_CASE("writer profile: report contents, counters, byte-identity", "[writer][profile]") {
    namespace fs = std::filesystem;
    auto root = make_profile_tree("arch_prof_tree");
    const auto arc_on  = fs::temp_directory_path() / "arch_prof_on.arc";
    const auto arc_off = fs::temp_directory_path() / "arch_prof_off.arc";
    fs::remove(arc_on); fs::remove(arc_off);

    std::string rep;
    {
        arch::writer w(arc_on, profile_opts(true, 4));
        w.add_tree(root);
        w.finalize();
        rep = w.profile_report();
    }
    {
        arch::writer w(arc_off, profile_opts(false, 4));
        w.add_tree(root);
        w.finalize();
        CHECK(w.profile_report().empty()); // profiling off -> empty report
    }

    CHECK_FALSE(rep.empty());
    CHECK(rep.find("parch create profile:") != std::string::npos);
    CHECK(rep.find("15 files") != std::string::npos); // 12 small + 3 dup copies
    CHECK(rep.find("compress") != std::string::npos);
    CHECK(rep.find("source read") != std::string::npos);
    CHECK(rep.find("tree walk") != std::string::npos);
    CHECK(rep.find("dedup 2 hits") != std::string::npos); // 3rd copy stored, 2 hits
    CHECK(rep.find("parch create hint:") != std::string::npos);

    // Profiling must not change the bytes on disk.
    CHECK(slurp(arc_on) == slurp(arc_off));

    fs::remove_all(root); fs::remove(arc_on); fs::remove(arc_off);
}

TEST_CASE("writer profile: serial fallback reports into the same stages", "[writer][profile]") {
    namespace fs = std::filesystem;
    auto root = make_profile_tree("arch_prof_tree_serial");
    const auto arc = fs::temp_directory_path() / "arch_prof_serial.arc";
    fs::remove(arc);

    auto o = profile_opts(true, 0); // threads = 0 -> serial fallback
    arch::writer w(arc, o);
    w.add_tree(root);
    w.finalize();
    const auto rep = w.profile_report();
    CHECK_FALSE(rep.empty());
    CHECK(rep.find("parch create profile:") != std::string::npos);
    CHECK(rep.find("source read") != std::string::npos);
    CHECK(rep.find("compress") != std::string::npos);
    CHECK(rep.find("dedup 2 hits") != std::string::npos);

    fs::remove_all(root); fs::remove(arc);
}

TEST_CASE("writer::contents(): entry counts + bytes from the buffered TOC", "[writer]") {
    namespace fs = std::filesystem;
    const auto rp = fs::temp_directory_path() / "arch_contents_tree";
    fs::remove_all(rp);
    const fs::path src = rp / "src", arc = rp / "t.arc";
    fs::create_directories(src / "sub");
    { std::ofstream(src / "a.txt") << "aaaa"; }     // 4 B
    { std::ofstream(src / "sub/b.txt") << "bb"; }   // 2 B
    fs::create_symlink("a.txt", src / "link");
    fs::create_hard_link(src / "a.txt", src / "sub/a2.txt"); // dup: own size counts

    // Pipelined and serial paths buffer the same entries: identical counts.
    for (int threads : {4, 0}) {
        fs::remove(arc);
        arch::writer::options o;
        o.threads = threads;
        arch::writer w(arc, o);
        w.add_tree(src, "src"); // tar-style prefix: "src" itself is a dir entry
        const auto s = w.contents();
        CHECK(s.files  == 3); // a.txt, sub/b.txt, sub/a2.txt (hardlink dup)
        CHECK(s.dirs   == 2); // src, src/sub
        CHECK(s.others == 1); // symlink
        CHECK(s.bytes  == 4 + 2 + 4);
        w.finalize();
    }
    fs::remove_all(rp);
}
