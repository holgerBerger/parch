// Phase 3: file-level I/O + compression pipeline tests.
// Cover ordered multi-block delivery, error surfacing, backpressure bounds,
// the uncompressed pass-through, and writer determinism (serial add_tree vs
// pipelined add_tree must produce byte-identical archives).
#include <catch2/catch_test_macros.hpp>
#include "arch/archive.h"
#include "arch/compressor.h"
#include "arch/format.h"
#include "arch/pipeline.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <vector>
#include <crc32c/crc32c.h>

using namespace arch;

namespace {

struct temp_dir {
    std::filesystem::path path;
    temp_dir() {
        char tmpl[] = "/tmp/opencode/arch_pipe_XXXXXX";
        const char* p = ::mkdtemp(tmpl);
        REQUIRE(p != nullptr);
        path = p;
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// Deterministic pseudo-random content (independent of the system RNG).
std::vector<std::uint8_t> make_content(std::size_t n, std::uint64_t seed) {
    std::vector<std::uint8_t> d(n);
    std::uint64_t s = seed ? seed : 1;
    for (std::size_t i = 0; i < n; ++i) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        d[i] = static_cast<std::uint8_t>(s >> 40);
    }
    return d;
}

void write_file(const std::filesystem::path& p, const std::vector<std::uint8_t>& d) {
    std::ofstream os(p, std::ios::binary | std::ios::trunc);
    REQUIRE(os.is_open());
    if (!d.empty()) os.write(reinterpret_cast<const char*>(d.data()),
                             static_cast<std::streamsize>(d.size()));
    REQUIRE(os.good());
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream is(p, std::ios::binary | std::ios::ate);
    REQUIRE(is.is_open());
    const auto n = static_cast<std::size_t>(is.tellg());
    is.seekg(0);
    std::vector<std::uint8_t> d(n);
    if (n) REQUIRE(static_cast<bool>(is.read(reinterpret_cast<char*>(d.data()),
                                             static_cast<std::streamsize>(n))));
    return d;
}

io::pipeline::options tight_opts(int files = 4, int comps = 3) {
    io::pipeline::options o;
    o.files_in_flight = files;
    o.compress_threads = comps;
    o.block_permits = 1;   // constructor must scale up to the liveness bound
    o.per_file_cap = 1;
    return o;
}

// Consume every block of `seq`, appending the raw-equivalent bytes (via the
// zstd decompressor) — verification is done by the caller per test.
struct drained {
    std::vector<std::uint8_t> blocks_joined;      // stored (compressed) bytes
    std::vector<std::uint32_t> uncompressed_sizes;
    io::pipeline_file fin;
};

drained drain(io::pipeline& p, std::uint64_t seq, bool decompress,
              std::string_view codec = "zstd", int level = 3) {
    drained out;
    std::unique_ptr<compress::compressor> comp;
    if (decompress) comp = compress::make_compressor(codec, level);
    std::size_t i = 0;
    io::pipeline_block pb;
    while (p.next_block(seq, i, pb)) {
        out.uncompressed_sizes.push_back(pb.uncompressed_size);
        if (decompress) {
            auto raw = comp->decompress(pb.compressed, pb.uncompressed_size);
            REQUIRE_FALSE(raw.empty());
            out.blocks_joined.insert(out.blocks_joined.end(), raw.begin(), raw.end());
        } else {
            out.blocks_joined.insert(out.blocks_joined.end(),
                                     pb.compressed.begin(), pb.compressed.end());
        }
        p.release_block(seq);
        pb = io::pipeline_block{};
        i++;
    }
    out.fin = p.wait_file_done(seq);
    return out;
}

} // namespace

TEST_CASE("pipeline: ordered multi-block delivery + checksums", "[pipeline]") {
    temp_dir td;
    const std::size_t n = 3 * format::ARCH_BLOCK_SIZE + 12345; // 4 blocks
    auto content = make_content(n, 0xC0FFEE);
    auto fp = td.path / "big.bin";
    write_file(fp, content);

    io::pipeline p(tight_opts(), "zstd", 3, /*use_compression=*/true, /*dedup=*/true);
    auto seq = p.submit(fp, n);
    auto d = drain(p, seq, /*decompress=*/true);

    REQUIRE_FALSE(d.fin.error);
    REQUIRE(d.fin.st.st_size == static_cast<off_t>(n));
    REQUIRE(d.blocks_joined == content);
    REQUIRE(d.uncompressed_sizes.size() == 4);
    REQUIRE(d.uncompressed_sizes[3] == 12345);
    REQUIRE(d.fin.file_crc32c ==
            crc32c::Crc32c(content.data(), content.size()));
}

TEST_CASE("pipeline: uncompressed pass-through", "[pipeline]") {
    temp_dir td;
    auto content = make_content(100000, 42);
    auto fp = td.path / "raw.bin";
    write_file(fp, content);

    io::pipeline p(tight_opts(), "zstd", 3, /*use_compression=*/false, /*dedup=*/false);
    auto seq = p.submit(fp, content.size());
    auto d = drain(p, seq, /*decompress=*/false);

    REQUIRE_FALSE(d.fin.error);
    REQUIRE(d.blocks_joined == content); // stored bytes are the raw bytes
    REQUIRE(d.uncompressed_sizes.size() == 1);
}

TEST_CASE("pipeline: open error surfaces via wait_file_done", "[pipeline]") {
    temp_dir td;
    auto good = td.path / "good.bin";
    write_file(good, make_content(4096, 7));

    io::pipeline p(tight_opts(), "zstd", 3, true, false);
    auto bad_seq = p.submit(td.path / "missing.bin", 10);
    io::pipeline_block pb;
    REQUIRE_FALSE(p.next_block(bad_seq, 0, pb));
    auto bad = p.wait_file_done(bad_seq);
    REQUIRE(bad.error == std::errc::no_such_file_or_directory);
    REQUIRE(bad.file_crc32c == 0);

    // The pipeline keeps working after a failed file.
    auto ok_seq = p.submit(good, 4096);
    auto d = drain(p, ok_seq, true);
    REQUIRE_FALSE(d.fin.error);
    REQUIRE(d.blocks_joined == make_content(4096, 7));
}

TEST_CASE("pipeline: empty file", "[pipeline]") {
    temp_dir td;
    auto fp = td.path / "empty";
    write_file(fp, {});

    io::pipeline p(tight_opts(), "zstd", 3, true, true);
    auto seq = p.submit(fp, 0);
    io::pipeline_block pb;
    REQUIRE_FALSE(p.next_block(seq, 0, pb));
    auto fin = p.wait_file_done(seq);
    REQUIRE_FALSE(fin.error);
    REQUIRE(fin.st.st_size == 0);
    REQUIRE(fin.file_crc32c == 0);
}

TEST_CASE("pipeline: in-flight window is bounded (backpressure)", "[pipeline]") {
    temp_dir td;
    constexpr int kFiles = 16;
    std::vector<std::filesystem::path> files;
    std::vector<std::vector<std::uint8_t>> contents;
    for (int i = 0; i < kFiles; ++i) {
        auto c = make_content(70000 + 131 * i, 1000 + i);
        auto fp = td.path / ("f" + std::to_string(i));
        write_file(fp, c);
        files.push_back(fp);
        contents.push_back(std::move(c));
    }

    constexpr int kDepth = 3;
    io::pipeline p(tight_opts(kDepth, 2), "zstd", 3, true, false);

    // Writer-style loop: submit ahead, drain the head when the window is full.
    std::deque<std::uint64_t> seqs;
    std::vector<std::vector<std::uint8_t>> got;
    for (int i = 0; i < kFiles; ++i) {
        while (p.in_flight() >= kDepth) {
            auto d = drain(p, seqs.front(), true);
            seqs.pop_front();
            REQUIRE_FALSE(d.fin.error);
            got.push_back(d.blocks_joined);
        }
        seqs.push_back(p.submit(files[i], contents[i].size()));
        REQUIRE(p.in_flight() <= static_cast<std::size_t>(kDepth));
    }
    while (!seqs.empty()) {
        auto d = drain(p, seqs.front(), true);
        seqs.pop_front();
        REQUIRE_FALSE(d.fin.error);
        got.push_back(d.blocks_joined);
    }
    REQUIRE(got.size() == static_cast<std::size_t>(kFiles));
    for (int i = 0; i < kFiles; ++i)
        REQUIRE(got[i] == contents[i]); // submit order is preserved
}

TEST_CASE("pipeline: destructor stops with unconsumed files", "[pipeline]") {
    temp_dir td;
    auto fp = td.path / "x.bin";
    write_file(fp, make_content(9 * format::ARCH_BLOCK_SIZE, 5));

    // Submit and destroy without consuming anything — must not hang.
    io::pipeline p(tight_opts(2, 2), "zstd", 3, true, true);
    p.submit(fp, 9 * format::ARCH_BLOCK_SIZE);
    p.submit(fp, 9 * format::ARCH_BLOCK_SIZE);
    // dtor joins workers while raw blocks are queued/unreleased.
}

TEST_CASE("writer add_tree: recovers after a failed file", "[pipeline][writer]") {
    if (geteuid() == 0) SKIP("permission model differs as root");
    namespace fs = std::filesystem;
    temp_dir td;
    auto root1 = td.path / "t1";
    fs::create_directories(root1);
    write_file(root1 / "ok1", make_content(65536, 1));
    write_file(root1 / "noperm", make_content(65536, 2));
    REQUIRE(::chmod((root1 / "noperm").c_str(), 0) == 0);

    writer::options opts;
    opts.compression_level = 3;
    opts.threads = 4;
    opts.pipeline_depth = 2;
    opts.durable = false;
    writer w(td.path / "rec.arc", opts);

    REQUIRE_THROWS_AS(w.add_tree(root1), std::system_error);
    REQUIRE(::chmod((root1 / "noperm").c_str(), 0644) == 0);

    // A second add_tree with more files than the pipeline window must not
    // hang: the failed call reaped its in-flight submissions.
    auto root2 = td.path / "t2";
    fs::create_directories(root2);
    for (int i = 0; i < 8; ++i)
        write_file(root2 / ("f" + std::to_string(i)), make_content(40000 + i, 10 + i));
    REQUIRE_NOTHROW(w.add_tree(root2));
    REQUIRE_NOTHROW(w.finalize());
}

TEST_CASE("writer add_tree: pipelined == serial, byte-identical", "[pipeline][writer]") {
    namespace fs = std::filesystem;
    temp_dir td;
    auto root = td.path / "tree";
    fs::create_directories(root / "sub" / "deep");

    write_file(root / "a.txt", make_content(5 * format::ARCH_BLOCK_SIZE + 7, 11));
    write_file(root / "b.txt", make_content(3, 22));
    write_file(root / "sub" / "c.bin", make_content(2 * format::ARCH_BLOCK_SIZE, 33));
    write_file(root / "sub" / "deep" / "d.bin", make_content(1000000, 44));
    write_file(root / "empty", {});
    write_file(root / "dup_src", make_content(60000, 55));
    fs::create_hard_link(root / "dup_src", root / "dup_link");
    fs::create_symlink("a.txt", root / "link_a");

    // Freeze timestamps before every archive write: reading the first
    // archive would otherwise bump atime (relatime) and break equality.
    auto freeze = [&] {
        for (auto& e : fs::recursive_directory_iterator(root)) {
            struct timespec times[2] = {{1700000000, 0}, {1700000001, 0}};
            if (::utimensat(AT_FDCWD, e.path().c_str(), times,
                            e.is_symlink() ? AT_SYMLINK_NOFOLLOW : 0) != 0)
                UNSCOPED_INFO("utimensat failed for " + e.path().string());
        }
    };

    auto write_arc = [&](const fs::path& out, bool pipelined, bool dedup) {
        freeze();
        writer::options opts;
        opts.compressor = "zstd";
        opts.compression_level = 3;
        opts.threads = pipelined ? 4 : 0;   // 0 → serial fallback
        opts.pipeline_depth = 2;            // stress the window logic
        opts.dedup = dedup;
        opts.durable = false;
        writer w(out, opts);
        w.add_tree(root);
        w.finalize();
    };

    SECTION("dedup off") {
        auto a = td.path / "serial.arc", b = td.path / "pipe.arc";
        write_arc(a, false, false);
        write_arc(b, true, false);
        REQUIRE(read_file(a) == read_file(b));
    }
    SECTION("dedup on") {
        auto a = td.path / "serial_dd.arc", b = td.path / "pipe_dd.arc";
        write_arc(a, false, true);
        write_arc(b, true, true);
        REQUIRE(read_file(a) == read_file(b));
    }
}
