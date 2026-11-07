// Phase 8: sparse-file archiving (v0.3 COMPRESSOR_SPARSE blocks), transient
// -error retry, and reader::verify_all(). Tags: [sparse] [retry] [verify].
#include <catch2/catch_test_macros.hpp>
#include "arch/archive.h"
#include "arch/format.h"
#include "io/sparse.h"
#include "util/retry.h"
#include <crc32c/crc32c.h>
#include <xxhash.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <ios>
#include <random>
#include <vector>

namespace {

constexpr std::uint64_t BLK = arch::format::ARCH_BLOCK_SIZE; // 4 MiB

struct sparse_tree {
    std::filesystem::path root, src, arc, out;
    std::vector<std::uint8_t> expected; // full logical content of big.bin
};

// Nonzero bytes only: tmpfs punches out all-zero pages written via pwrite,
// so "data" regions sprinkled with 0x00 are not reliably dense.
std::vector<std::uint8_t> pseudo(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::uint8_t> v(n);
    for (auto& b : v) { std::uint64_t r = rng(); std::memcpy(&b, &r, 1); b |= 1; }
    return v;
}

// Does this filesystem answer SEEK_HOLE/SEEK_DATA (tmpfs/ext4/btrfs do;
// some network mounts report everything as one big hole, which is correct
// but makes hole-skipping untestable)?
bool fs_supports_hole_queries() {
    static const bool ok = [] {
        char tmpl[] = "/tmp/opencode/arch_probe_XXXXXX";
        const int t = ::mkstemp(tmpl);
        if (t < 0) return false;
        ::close(t);
        const int fd = ::open(tmpl, O_RDWR | O_TRUNC);
        bool supported = false;
        if (fd >= 0) {
            const auto d = pseudo(4096, 12345);
            if (::pwrite(fd, d.data(), d.size(), 0) == 4096 &&
                ::ftruncate(fd, 1024 * 1024) == 0)
                supported = arch::io::sparse::block_is_hole(fd, 4096, 1024);
            ::close(fd);
        }
        ::unlink(tmpl);
        return supported;
    }();
    return ok;
}


// big.bin layout (5 whole blocks): block 0 = data | blocks 1,2,3 = holes |
// block 4 = data. Plus a dense small.txt. Cleans up via the path tree.
sparse_tree make_tree(const char* tag) {
    char tmpl[128];
    std::snprintf(tmpl, sizeof tmpl, "/tmp/opencode/arch_sparse_%s_XXXXXX", tag);
    const char* d = ::mkdtemp(tmpl);
    REQUIRE(d != nullptr);
    sparse_tree t;
    t.root = d;
    t.src = t.root / "src";
    t.arc = t.root / "t.arc";
    t.out = t.root / "out";
    std::filesystem::create_directories(t.src);

    auto head = pseudo(BLK, 2); // block 0
    auto mid = pseudo(BLK, 4);  // block 4
    t.expected = head;
    t.expected.insert(t.expected.end(), 3 * BLK, 0); // blocks 1-3: holes
    t.expected.insert(t.expected.end(), mid.begin(), mid.end());

    int fd = ::open((t.src / "big.bin").c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    REQUIRE(fd >= 0);
    REQUIRE(::pwrite(fd, head.data(), head.size(), 0) == (ssize_t)head.size());
    REQUIRE(::pwrite(fd, mid.data(), mid.size(), static_cast<off_t>(4 * BLK)) ==
            (ssize_t)mid.size());
    ::close(fd);
    struct stat st{};
    REQUIRE(::stat((t.src / "big.bin").c_str(), &st) == 0);
    REQUIRE(static_cast<std::uint64_t>(st.st_size) == 5 * BLK);
    if (!fs_supports_hole_queries())
        SKIP("filesystem lacks SEEK_HOLE/SEEK_DATA — cannot test sparse");

    { std::ofstream(t.src / "small.txt") << "dense content, no holes here\n"; }
    return t;
}

std::uint16_t footer_version(const std::filesystem::path& arc) {
    std::ifstream in(arc, std::ios::binary | std::ios::ate);
    const auto n = static_cast<std::streamoff>(in.tellg());
    REQUIRE(n > 256);
    std::vector<std::uint8_t> f(256);
    in.seekg(n - 256);
    in.read(reinterpret_cast<char*>(f.data()), 256);
    return static_cast<std::uint16_t>((f[4] << 8) | f[5]);
}

std::uint64_t file_bytes(const std::filesystem::path& p) {
    std::error_code ec;
    return static_cast<std::uint64_t>(std::filesystem::file_size(p, ec));
}

std::vector<std::uint8_t> read_all(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>()};
}

// big.bin keeps 8 MiB of incompressible pseudo-random data; only the 12 MiB
// hole run must disappear from the archive.
constexpr std::uint64_t SPARSE_RT_MAX = 9 * 1024 * 1024;

} // namespace

// ---------------------------------------------------------------------------
// roundtrips
// ---------------------------------------------------------------------------

TEST_CASE("sparse: pipeline write skips holes, extract is byte-identical", "[sparse]") {
    auto t = make_tree("rt");
    {
        arch::writer w(t.arc); // sparse on by default, threads default → pipeline
        w.add_tree(t.src);
        w.finalize();
    }
    CHECK(footer_version(t.arc) == 0x0003);
    CHECK(file_bytes(t.arc) < SPARSE_RT_MAX); // 12 MiB of holes not stored

    {
        arch::reader r(t.arc);
        CHECK(r.verify_all().empty());
        r.extract_all(t.out);
    }
    const auto got = read_all(t.out / "big.bin");
    REQUIRE(got.size() == t.expected.size());
    CHECK(got == t.expected);
    // Extracted file stays sparse: allocated bytes far below the 20 MiB
    // logical size (only the two 4 MiB data blocks are written).
    struct stat st{};
    REQUIRE(::stat((t.out / "big.bin").c_str(), &st) == 0);
    CHECK(static_cast<std::uint64_t>(st.st_blocks) * 512 <= 9 * 1024 * 1024);
    std::filesystem::remove_all(t.root);
}

TEST_CASE("sparse: partial block starting in data is never marked sparse", "[sparse]") {
    auto t = make_tree("partial");
    // part.bin: one page at 0, zeros (written, then hole-punched on tmpfs)
    // to the end of block 0, data page in block 1 → block 0 STARTS on data
    // and must be stored as a regular block whatever the fs thinks.
    std::vector<std::uint8_t> want(BLK + 4096, 0);
    auto a = pseudo(4096, 5), b = pseudo(4096, 6);
    std::copy(a.begin(), a.end(), want.begin());
    std::copy(b.begin(), b.end(), want.begin() + BLK);
    {
        int fd = ::open((t.src / "part.bin").c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        REQUIRE(fd >= 0);
        REQUIRE(::pwrite(fd, a.data(), a.size(), 0) == 4096);
        REQUIRE(::pwrite(fd, b.data(), b.size(), static_cast<off_t>(BLK)) == 4096);
        ::close(fd);
    }
    {
        arch::writer w(t.arc);
        w.add_tree(t.src);
        w.finalize();
    }
    { arch::reader r(t.arc); r.extract_all(t.out); }
    CHECK(read_all(t.out / "part.bin") == want);
    std::filesystem::remove_all(t.root);
}

TEST_CASE("sparse: trailing hole restored via ftruncate", "[sparse]") {
    auto t = make_tree("tail");
    // hole2.bin: one data page at 0, then a hole that ENDS the file (3 blocks).
    const std::size_t dsz = 4096;
    auto head = pseudo(dsz, 7);
    int fd = ::open((t.src / "hole2.bin").c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    REQUIRE(fd >= 0);
    REQUIRE(::pwrite(fd, head.data(), dsz, 0) == (ssize_t)dsz);
    REQUIRE(::ftruncate(fd, static_cast<off_t>(3 * BLK)) == 0); // trailing hole
    ::close(fd);
    {
        arch::writer w(t.arc);
        w.add_tree(t.src);
        w.finalize();
    }
    { arch::reader r(t.arc); CHECK(r.verify_all().empty()); r.extract_all(t.out); }
    struct stat st{};
    REQUIRE(::stat((t.out / "hole2.bin").c_str(), &st) == 0);
    CHECK(static_cast<std::uint64_t>(st.st_size) == 3 * BLK); // size preserved
    const auto got = read_all(t.out / "hole2.bin");
    REQUIRE(got.size() == 3 * BLK);
    CHECK(std::equal(head.begin(), head.end(), got.begin()));
    CHECK(std::all_of(got.begin() + dsz, got.end(), [](std::uint8_t b) { return b == 0; }));
    std::filesystem::remove_all(t.root);
}

TEST_CASE("sparse: options.sparse=false stores holes densely (v0.2 footer)", "[sparse]") {
    auto t = make_tree("off");
    arch::writer::options o;
    o.sparse = false;
    {
        arch::writer w(t.arc, o);
        w.add_tree(t.src);
        w.finalize();
    }
    CHECK(footer_version(t.arc) == 0x0002);
    // 12 MiB of zeros ARE in the archive (highly compressible but present):
    // strictly bigger than the sparse archive of the same tree.
    const auto dense_sz = file_bytes(t.arc);
    const std::filesystem::path arc2 = t.root / "t2.arc";
    {
        arch::writer w(arc2); // sparse default on
        w.add_tree(t.src);
        w.finalize();
    }
    CHECK(dense_sz > file_bytes(arc2));
    { arch::reader r(t.arc); r.extract_all(t.out); }
    CHECK(read_all(t.out / "big.bin") == t.expected);
    std::filesystem::remove_all(t.root);
}

TEST_CASE("sparse: serial path (threads=0) matches pipeline bytes", "[sparse]") {
    auto t = make_tree("serial");
    arch::writer::options o;
    o.threads = 0; // serial fallback in add_file_impl
    {
        arch::writer w(t.arc, o);
        w.add_tree(t.src);
        w.finalize();
    }
    CHECK(footer_version(t.arc) == 0x0003);
    CHECK(file_bytes(t.arc) < SPARSE_RT_MAX);
    { arch::reader r(t.arc); r.extract_all(t.out); }
    CHECK(read_all(t.out / "big.bin") == t.expected);
    std::filesystem::remove_all(t.root);
}

TEST_CASE("sparse: store mode + dedup cross-dedups holes and zero blocks", "[sparse][dedup]") {
    char tmpl[128];
    std::snprintf(tmpl, sizeof tmpl, "/tmp/opencode/arch_dedup_%s_XXXXXX", "d");
    const char* d = ::mkdtemp(tmpl);
    REQUIRE(d != nullptr);
    const std::filesystem::path root(d), src = root / "src", arc = root / "t.arc",
                                  out = root / "out";
    std::filesystem::create_directories(src);

    // sparse_src.bin: 4 hole blocks + one data page starting block 4 (the
    // file's only data; a block starting ON data stores whole, so it goes
    // last). zero.bin's punched-zero blocks dedup against the hole owners.
    auto head = pseudo(4096, 11);
    {
        int fd = ::open((src / "sparse_src.bin").c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        REQUIRE(fd >= 0);
        REQUIRE(::ftruncate(fd, static_cast<off_t>(4 * BLK + 4096)) == 0);
        REQUIRE(::pwrite(fd, head.data(), head.size(), static_cast<off_t>(4 * BLK)) ==
                (ssize_t)head.size());
        ::close(fd);
    }
    // zero.bin: 3 DENSE blocks of zeros (same bytes as the holes' content).
    const std::vector<std::uint8_t> zeros(BLK, 0);
    {
        int fd = ::open((root / "zero.bin").c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        REQUIRE(fd >= 0);
        for (int i = 0; i < 3; ++i)
            REQUIRE(::pwrite(fd, zeros.data(), zeros.size(),
                             static_cast<off_t>(i * BLK)) == (ssize_t)BLK);
        ::close(fd);
        struct stat st{};
        REQUIRE(::stat((root / "zero.bin").c_str(), &st) == 0);
        REQUIRE(static_cast<std::uint64_t>(st.st_size) == 3 * BLK);
        // Whether tmpfs kept these zero pages dense or punched them into
        // holes, every block is all-zero ⇒ same content_id either way.
    }
    if (!fs_supports_hole_queries()) SKIP("no SEEK_HOLE support");
    arch::writer::options o;
    o.compressor = "none"; // raw storage: 12 MiB un-deduped would stay 12 MiB
    o.dedup = true;
    o.threads = 0;         // serial path owns the hole blocks here
    {
        arch::writer w(arc, o);
        w.add_tree(src);                     // holes → sparse owner blocks
        w.add_file(root / "zero.bin");       // dense zeros → dedup hits
        w.finalize();
    }
    CHECK(file_bytes(arc) < 1024 * 1024); // ~8 KiB of real data + metadata
    {
        arch::reader r(arc);
        CHECK(r.verify_all().empty());
        r.extract_all(out);
    }
    const auto z = read_all(out / "zero.bin");
    REQUIRE(z.size() == 3 * BLK);
    CHECK(std::all_of(z.begin(), z.end(), [](std::uint8_t b) { return b == 0; }));
    const auto s = read_all(out / "sparse_src.bin");
    REQUIRE(s.size() == 4 * BLK + 4096);
    CHECK(std::all_of(s.begin(), s.begin() + 4 * BLK,
                      [](std::uint8_t b) { return b == 0; }));
    CHECK(std::equal(head.begin(), head.end(), s.begin() + 4 * BLK));
    std::filesystem::remove_all(root);
}

TEST_CASE("sparse: append keeps advertising v0.3, extraction intact", "[sparse][append]") {
    auto t = make_tree("app");
    {
        arch::writer w(t.arc);
        w.add_file(t.src / "small.txt"); // dense section → v0.2
        w.finalize();
    }
    CHECK(footer_version(t.arc) == 0x0002);
    {
        auto w = arch::writer::append(t.arc);
        w.add_file(t.src / "big.bin");   // sparse section → sticky v0.3
        w.finalize();
    }
    CHECK(footer_version(t.arc) == 0x0003);
    {
        auto w = arch::writer::append(t.arc);
        w.add_file(t.src / "small.txt");
        w.finalize();
    }
    CHECK(footer_version(t.arc) == 0x0003); // stays 0.3 after dense re-append

    arch::reader r(t.arc);
    CHECK(r.verify_all().empty());
    r.extract_all(t.out);
    CHECK(read_all(t.out / "big.bin") == t.expected);
    std::filesystem::remove_all(t.root);
}

// ---------------------------------------------------------------------------
// helpers: zero_digest / crc_extend_zeros / block_is_hole
// ---------------------------------------------------------------------------

TEST_CASE("sparse: helpers match their dense equivalents", "[sparse]") {
    // zero_digest == XXH3-128 of the same number of actual zero bytes.
    {
        const std::vector<std::uint8_t> z(1 << 16, 0);
        XXH128_hash_t h = XXH3_128bits(z.data(), z.size());
        arch::format::digest128 want{};
        std::memcpy(want.data(), &h.low64, 8);
        std::memcpy(want.data() + 8, &h.high64, 8);
        CHECK(arch::io::sparse::zero_digest(z.size()) == want);
    }
    // crc_extend_zeros == CRC over an actual zero buffer.
    {
        const std::vector<std::uint8_t> z(100000, 0);
        CHECK(arch::io::sparse::crc_extend_zeros(0, z.size()) ==
              crc32c::Crc32c(z.data(), z.size()));
        CHECK(arch::io::sparse::crc_extend_zeros(0x1234, 0) == 0x1234);
    }
    // block_is_hole on a hand-punched file (fresh open(): SEEK_* queries do
    // not depend on the fd offset, but mkstemp's fd would still be at 0).
    char tmpl[] = "/tmp/opencode/arch_hole_XXXXXX";
    { const int t = ::mkstemp(tmpl); REQUIRE(t >= 0); ::close(t); }
    const std::filesystem::path p(tmpl);
    int fd = ::open(tmpl, O_RDWR | O_TRUNC);
    REQUIRE(fd >= 0);
    const auto d = pseudo(4096, 8);
    REQUIRE(::pwrite(fd, d.data(), d.size(), 0) == 4096);
    REQUIRE(::pwrite(fd, d.data(), d.size(), 1024 * 1024) == 4096);
    CHECK(arch::io::sparse::block_is_hole(fd, 4096, 1024));           // interior hole
    CHECK_FALSE(arch::io::sparse::block_is_hole(fd, 0, 1024));        // starts on data
    CHECK(arch::io::sparse::block_is_hole(fd, 4096, 1024 * 1024 - 4096)); // whole run
    CHECK_FALSE(arch::io::sparse::block_is_hole(fd, 0, 1024 * 1024)); // straddles data
    CHECK(arch::io::sparse::block_is_hole(fd, 2 * 1024 * 1024, 4096)); // past data ⇒ ENXIO
    ::close(fd);
    std::filesystem::remove(p);
}

// ---------------------------------------------------------------------------
// retry (Phase 8 item 31)
// ---------------------------------------------------------------------------

TEST_CASE("retry: transient errors retried with backoff, then success", "[retry]") {
    using namespace arch::detail;
    const auto base = retry_sleep_count().load();
    int calls = 0;
    auto op = [&]() -> ssize_t {
        if (++calls < 3) { errno = EIO; return -1; }
        return 42;
    };
    CHECK(retry_io(op) == 42);
    CHECK(calls == 3);
    CHECK(retry_sleep_count() == base + 2); // two backoffs for two failures
}

TEST_CASE("retry: permanent errors are not retried", "[retry]") {
    using namespace arch::detail;
    const auto base = retry_sleep_count().load();
    int calls = 0;
    auto op = [&]() -> ssize_t { ++calls; errno = ENOENT; return -1; };
    CHECK(retry_io(op) == -1);
    CHECK(errno == ENOENT);
    CHECK(calls == 1);
    CHECK(retry_sleep_count() == base);
}

TEST_CASE("retry: EINTR is free, attempt cap ends with the errno", "[retry]") {
    using namespace arch::detail;
    const auto base = retry_sleep_count().load();
    int calls = 0;
    auto op = [&]() -> ssize_t {
        ++calls;
        errno = calls % 2 ? EINTR : ESTALE; // alternate: free, then counted
        return -1;
    };
    // 3 counted attempts: ESTALE #1 #2 #3 backoff, #4 hits the cap → -1.
    // Between them 4 EINTR calls. Total 8 calls, 3 backoffs.
    CHECK(retry_io(op, 3) == -1);
    CHECK(errno == ESTALE);
    CHECK(retry_sleep_count() == base + 3);
    CHECK(calls == 8);
}

TEST_CASE("retry: is_transient_errno classification", "[retry]") {
    using namespace arch::detail;
    CHECK(is_transient_errno(EIO));
    CHECK(is_transient_errno(ESTALE));
    CHECK(is_transient_errno(EAGAIN));
    CHECK(is_transient_errno(ETIMEDOUT));
    CHECK_FALSE(is_transient_errno(ENOENT));
    CHECK_FALSE(is_transient_errno(EACCES));
    CHECK_FALSE(is_transient_errno(EINVAL));
}

// ---------------------------------------------------------------------------
// verify_all (Phase 8 item 29)
// ---------------------------------------------------------------------------

TEST_CASE("verify: detects corrupted data block by name and index", "[verify]") {
    auto t = make_tree("vbad");
    {
        arch::writer w(t.arc);
        w.add_tree(t.src);
        w.finalize();
    }
    // Flip a byte inside big.bin's first data block (offset 200 > header+128).
    {
        int fd = ::open(t.arc.c_str(), O_RDWR);
        REQUIRE(fd >= 0);
        std::uint8_t b;
        REQUIRE(::pread(fd, &b, 1, 200) == 1);
        b ^= 0xFF;
        REQUIRE(::pwrite(fd, &b, 1, 200) == 1);
        ::close(fd);
    }
    arch::reader r(t.arc); // metadata CRC covers tables only — must open
    const auto errs = r.verify_all();
    REQUIRE(errs.size() == 1);
    CHECK(errs[0].find("big.bin") != std::string::npos);
    CHECK(errs[0].find("block 0") != std::string::npos);
    std::filesystem::remove_all(t.root);
}

TEST_CASE("verify: clean archive reports no errors, hardlinks verified once", "[verify]") {
    auto t = make_tree("vok");
    std::filesystem::create_hard_link(t.src / "small.txt", t.src / "small2.txt");
    {
        arch::writer w(t.arc);
        w.add_tree(t.src);
        w.finalize();
    }
    {
        arch::reader r(t.arc);
        CHECK(r.verify_all().empty());
    }
    // Second archive: ONLY the hardlink pair, added via add_tree (that is
    // where the writer recognizes the dup). Corrupt its one data block
    // (section starts at 16, 128-byte toc_header, data at 144): both the
    // owner and the hardlink dup must be reported (dup verifies through the
    // owner's blocks).
    std::filesystem::remove(t.arc);
    const std::filesystem::path hl = t.root / "hl";
    std::filesystem::create_directories(hl);
    std::filesystem::copy_file(t.src / "small.txt", hl / "a.txt");
    std::filesystem::create_hard_link(hl / "a.txt", hl / "b.txt");
    {
        arch::writer w(t.arc);
        w.add_tree(hl);
        w.finalize();
    }
    {
        int fd = ::open(t.arc.c_str(), O_RDWR);
        REQUIRE(fd >= 0);
        std::uint8_t b;
        REQUIRE(::pread(fd, &b, 1, 160) == 1); // inside the stored block
        b ^= 0xFF;
        REQUIRE(::pwrite(fd, &b, 1, 160) == 1);
        ::close(fd);
    }
    arch::reader r(t.arc); // metadata stays intact — must open
    const auto errs = r.verify_all();
    REQUIRE(errs.size() == 2);
    CHECK(errs[0].find("a.txt") != std::string::npos);
    CHECK(errs[1].find("b.txt") != std::string::npos);
    std::filesystem::remove_all(t.root);
}
