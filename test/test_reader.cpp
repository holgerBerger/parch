// Phase 4: reader tests — list(), extract/extract_all, corruption detection,
// truncation tolerance, and writer::append() round-trips. Parsers here are
// deliberately independent of both writer and reader serializers.
#include <catch2/catch_test_macros.hpp>
#include "arch/archive.h"
#include "arch/format.h"
#include "../src/archive/format_parse.h" // check_member_name (zip-slip)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <utime.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <crc32c/crc32c.h>

using namespace arch;

namespace fs = std::filesystem;

namespace {

struct temp_dir {
    std::filesystem::path path;
    temp_dir() {
        char tmpl[] = "/tmp/opencode/arch_read_XXXXXX";
        const char* p = ::mkdtemp(tmpl);
        REQUIRE(p != nullptr);
        path = p;
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

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
    std::filesystem::create_directories(p.parent_path());
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

// Freeze source mtimes so extraction can be compared exactly.
void freeze_time(const std::filesystem::path& p) {
    timespec ts[2] = {{1000000000, 0}, {1000000005, 123456789}}; // atime, mtime
    REQUIRE(::utimensat(AT_FDCWD, p.c_str(), ts, AT_SYMLINK_NOFOLLOW) == 0);
}

std::uint64_t fsize(const std::filesystem::path& p) {
    struct stat st{};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    return static_cast<std::uint64_t>(st.st_size);
}

// Open an archive file for byte-level patching in tests.
struct file_io {
    int fd;
    std::uint64_t size;
    explicit file_io(const std::filesystem::path& p) {
        fd = ::open(p.c_str(), O_RDWR);
        REQUIRE(fd >= 0);
        struct stat st{};
        REQUIRE(::fstat(fd, &st) == 0);
        size = static_cast<std::uint64_t>(st.st_size);
    }
    ~file_io() { ::close(fd); }
    std::vector<std::uint8_t> read(std::uint64_t off, std::size_t n) {
        std::vector<std::uint8_t> b(n);
        std::size_t got = 0;
        while (got < n) {
            ssize_t r = ::pread(fd, b.data() + got, n - got,
                                static_cast<off_t>(off + got));
            REQUIRE(r > 0);
            got += static_cast<std::size_t>(r);
        }
        return b;
    }
    void write(std::uint64_t off, const void* p, std::size_t n) {
        REQUIRE(::pwrite(fd, p, n, static_cast<off_t>(off)) ==
                static_cast<ssize_t>(n));
    }
    void patch_u64(std::uint64_t off, std::uint64_t v) {
        auto be = format::htobe64_safe(v);
        write(off, &be, 8);
    }
    void patch_u32(std::uint64_t off, std::uint32_t v) {
        auto be = format::htobe32_safe(v);
        write(off, &be, 4);
    }
    // Byte-flip at absolute file offset (touches one byte).
    void flip(std::uint64_t off) {
        auto b = read(off, 1);
        b[0] ^= 0xFF;
        write(off, b.data(), 1);
    }
    // Footer geometry helpers (256 bytes at EOF-256).
    std::uint64_t footer_off() const { return size - format::FOOTER_SIZE; }
    // Recompute footer toc_table_crc/footer_crc/signature_crc after patching
    // the TOC table, and re-derive them from the current bytes.
    void fixup_footer(std::uint64_t table_off, std::size_t table_len) {
        auto tbl = read(table_off, table_len);
        const std::uint32_t tcrc = crc32c::Crc32c(tbl.data(), tbl.size());
        patch_u32(footer_off() + 40, tcrc);
        auto f = read(footer_off(), format::FOOTER_SIZE);
        std::uint32_t fcrc = crc32c::Extend(0, f.data(), 44);
        fcrc = crc32c::Extend(fcrc, f.data() + 48, 200);
        patch_u32(footer_off() + 44, fcrc);
        f = read(footer_off(), format::FOOTER_SIZE);
        patch_u32(footer_off() + 248, crc32c::Crc32c(f.data(), 248));
    }
};

// Build the standard test tree: dirs, files, multi-block file, empty file,
// symlink, hardlink pair, xattr. Returns {abs paths, contents, rel names}.
struct tree_spec {
    std::filesystem::path root;
    std::map<std::string, std::vector<std::uint8_t>> contents; // rel -> bytes
};

tree_spec make_tree(const std::filesystem::path& base) {
    tree_spec t;
    t.root = base / "src";
    const auto big = make_content(5u * 1024 * 1024 + 7, 0xBEEF); // 2 blocks
    const auto small = make_content(4096, 1);
    t.contents["docs/readme.md"] = small;
    t.contents["docs/sub/notes.txt"] = make_content(1234, 2);
    t.contents["big.bin"] = big;
    t.contents["empty.dat"] = {};
    t.contents["xattr.bin"] = make_content(64, 3);
    for (auto& [rel, d] : t.contents) write_file(t.root / rel, d);

    std::filesystem::create_symlink("readme.md", t.root / "docs/link.md");
    std::filesystem::create_hard_link(t.root / "docs/readme.md",
                                      t.root / "docs/readme.hard");
    REQUIRE(::setxattr((t.root / "xattr.bin").c_str(), "user.arch.test",
                       "hello", 5, 0) == 0);
    for (auto& [rel, d] : t.contents) freeze_time(t.root / rel);
    freeze_time(t.root / "docs");
    return t;
}

std::filesystem::path archive_in(const temp_dir& td, const char* name = "arc.arch") {
    return td.path / name;
}

} // namespace

// ---- zip-slip guard (unit level) ----

TEST_CASE("reader zip-slip check rejects escaping names", "[reader][unit]") {
    CHECK_NOTHROW(arch::detail::check_member_name("a/b/c.txt"));
    CHECK_NOTHROW(arch::detail::check_member_name("..data"));   // not an element
    CHECK_NOTHROW(arch::detail::check_member_name("a..b/c"));
    CHECK_THROWS_AS(arch::detail::check_member_name("../x"), std::runtime_error);
    CHECK_THROWS_AS(arch::detail::check_member_name("a/../../b"), std::runtime_error);
    CHECK_THROWS_AS(arch::detail::check_member_name(".."), std::runtime_error);
    CHECK_THROWS_AS(arch::detail::check_member_name("x/.."), std::runtime_error);
    CHECK_THROWS_AS(arch::detail::check_member_name("/abs"), std::runtime_error);
    CHECK_THROWS_AS(arch::detail::check_member_name(""), std::runtime_error);
}

// ---- list() ----

TEST_CASE("reader list roundtrip: types, sizes, modes, times, links", "[reader]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w(arc);
        w.add_tree(t.root);
        w.finalize();
    }

    reader r(arc);
    auto listing = r.list();

    std::map<std::string, reader::listing_entry> by_name;
    for (auto& le : listing) by_name[le.path.generic_string()] = le;

    // 5 files + 1 symlink + 1 hardlink dup + 2 dirs (docs, docs/sub)
    CHECK(by_name.size() == listing.size());
    REQUIRE(by_name.count("big.bin") == 1);
    REQUIRE(by_name.count("docs/readme.md") == 1);
    REQUIRE(by_name.count("docs/link.md") == 1);
    REQUIRE(by_name.count("docs/readme.hard") == 1);
    REQUIRE(by_name.count("docs") == 1);
    REQUIRE(by_name.count("docs/sub") == 1);

    CHECK(by_name["big.bin"].type == format::file_type::regular);
    CHECK(by_name["big.bin"].size == 5u * 1024 * 1024 + 7);
    CHECK(by_name["empty.dat"].size == 0);
    CHECK(by_name["docs"].type == format::file_type::directory);
    CHECK(by_name["docs/link.md"].type == format::file_type::symlink);
    CHECK(by_name["docs/link.md"].link_target == "readme.md");
    // Walk order is alphabetical: "docs/readme.hard" < "docs/readme.md", so
    // the *hard* copy is the canonical owner and the other references it.
    CHECK(by_name["docs/readme.md"].link_target == "docs/readme.hard");
    CHECK(by_name["docs/readme.hard"].link_target.empty());
    struct stat src{};
    REQUIRE(::stat((t.root / "docs/readme.md").c_str(), &src) == 0);
    CHECK((by_name["docs/readme.md"].mode & 07777) == (src.st_mode & 07777));
    CHECK(by_name["docs/readme.md"].mtime == static_cast<std::uint64_t>(src.st_mtime));
}

// ---- extract_all ----

TEST_CASE("reader extract_all restores content, links, metadata", "[reader]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w(arc);
        w.add_tree(t.root);
        w.finalize();
    }

    const auto out = td.path / "out";
    {
        reader r(arc);
        r.extract_all(out);
    }

    for (auto& [rel, d] : t.contents)
        CHECK(read_file(out / rel) == d);

    // symlink + hardlink identity (canonical owner is readme.hard, see above)
    CHECK(std::filesystem::is_symlink(out / "docs/link.md"));
    CHECK(std::filesystem::read_symlink(out / "docs/link.md") == "readme.md");
    struct stat a{}, b{};
    REQUIRE(::stat((out / "docs/readme.md").c_str(), &a) == 0);
    REQUIRE(::stat((out / "docs/readme.hard").c_str(), &b) == 0);
    CHECK(a.st_ino == b.st_ino);
    CHECK(a.st_dev == b.st_dev);

    // mode + mtime restore (compare against the source, umask may vary)
    struct stat src{};
    REQUIRE(::stat((t.root / "docs/readme.md").c_str(), &src) == 0);
    CHECK((a.st_mode & 07777) == (src.st_mode & 07777));
    CHECK(a.st_mtim.tv_sec == 1000000005);
    CHECK(a.st_mtim.tv_nsec == 123456789);

    // xattr restored on disk AND readable from the archive
    char val[16] = {};
    ssize_t n = ::getxattr((out / "xattr.bin").c_str(), "user.arch.test", val, 15);
    REQUIRE(n == 5);
    CHECK(std::string(val, 5) == "hello");
    reader r(arc);
    auto xa = r.get_xattrs("xattr.bin");
    REQUIRE(xa.size() == 1);
    CHECK(xa[0].first == "user.arch.test");
    CHECK(std::string(xa[0].second.begin(), xa[0].second.end()) == "hello");
    CHECK(r.get_xattrs("big.bin").empty());
}

TEST_CASE("reader extract_all honours options.threads", "[reader]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w(arc);
        w.add_tree(t.root);
        w.finalize();
    }
    const auto out = td.path / "out4";
    reader::options ro;
    ro.threads = 4;
    reader r(arc, ro);
    r.extract_all(out);
    for (auto& [rel, d] : t.contents)
        CHECK(read_file(out / rel) == d);
}

TEST_CASE("reader extract single entry (incl. multi-block)", "[reader]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w(arc);
        w.add_tree(t.root);
        w.finalize();
    }
    reader r(arc);
    const auto dest = td.path / "one" / "big.bin"; // parent dir must be created
    r.extract("big.bin", dest);
    CHECK(read_file(dest) == t.contents["big.bin"]);
    CHECK_THROWS_AS(r.extract("no/such/file", td.path / "x"), std::runtime_error);
}

// ---- corruption detection ----

TEST_CASE("reader: corrupted data byte fails block CRC on extract", "[reader]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w(arc);
        w.add_tree(t.root);
        w.finalize();
    }
    { // flip one byte inside the first section's data region (offset 128+16+5 =
      // archive header (16) + toc_header (128) → first block byte at 149)
        file_io f(arc);
        f.flip(format::ARCHIVE_HEADER_SIZE + format::TOC_HEADER_SIZE + 5);
    }
    reader r(arc);                       // metadata still valid — opens fine
    CHECK_THROWS_AS(
        r.extract_all(td.path / "out"), std::runtime_error);
}

TEST_CASE("reader: corrupted metadata byte fails CRC on open", "[reader]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w(arc);
        w.add_tree(t.root);
        w.finalize();
    }
    { // flip a file-entry byte: the entry table starts right after the data
      // region; instead flip inside the footer TOC count to keep it simple —
      // patch the last byte of the string table (a name) via offset scan.
        file_io f(arc);
        auto data = f.read(0, f.size);
        const std::string needle = "big.bin";
        // last occurrence = the string table (earlier bytes are data)
        auto it = std::find_end(data.begin(), data.end(), needle.begin(), needle.end());
        REQUIRE(it != data.end());
        f.flip(static_cast<std::uint64_t>(it - data.begin()) + 2);
    }
    CHECK_THROWS_AS(reader(arc), std::runtime_error);
}

TEST_CASE("reader: bad footer and tiny file rejected", "[reader]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w(arc);
        w.add_tree(t.root);
        w.finalize();
    }
    // Bad footer magic (also breaks signature CRC): must throw.
    {
        file_io f(arc);
        f.flip(f.footer_off());
    }
    CHECK_THROWS_AS(reader(arc), std::runtime_error);

    // Too-small file.
    auto tiny = td.path / "tiny.arch";
    {
        std::ofstream os(tiny, std::ios::binary);
        os << std::string(42, 'x');
    }
    CHECK_THROWS_AS(reader(tiny), std::runtime_error);

    // Non-existent file: system_error (ENOENT).
    CHECK_THROWS_AS(reader(td.path / "missing.arch"), std::system_error);
}

// ---- truncation tolerance ----

TEST_CASE("reader drops a TOC section whose ref points past the table", "[reader]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w1(arc);
        w1.add_tree(t.root);
        w1.finalize();
        auto w2 = writer::append(arc);
        std::filesystem::create_directories(td.path / "more");
        write_file(td.path / "more" / "late.txt", make_content(100, 9));
        w2.add_tree(td.path / "more");
        w2.finalize();
    }
    { // Two sections: pretend section 2's finalize crashed — blow up its
      // stored size so it overlaps the TOC table, then repair the CRC chain.
        file_io f(arc);
        auto fbytes = f.read(f.footer_off(), format::FOOTER_SIZE);
        auto foot = arch::detail::parse_footer(fbytes.data());
        REQUIRE(foot.toc_count == 2);
        const auto table_len = static_cast<std::size_t>(foot.toc_count *
                                                        format::TOC_SECTION_REF_SIZE);
        const std::uint64_t last_ref =
            foot.toc_table_offset + format::TOC_SECTION_REF_SIZE; // 2nd ref
        f.patch_u64(last_ref + 8, f.size); // size = whole file → past the table
        f.fixup_footer(foot.toc_table_offset, table_len);
    }
    reader r(arc);
    auto lst = r.list();
    bool has_late = false, has_big = false;
    for (auto& le : lst) {
        if (le.path.generic_string() == "late.txt") has_late = true;
        if (le.path.generic_string() == "big.bin") has_big = true;
    }
    CHECK(has_big);    // section 1 intact
    CHECK(!has_late);  // section 2 dropped cleanly
    const auto out = td.path / "out";
    r.extract_all(out);
    CHECK(read_file(out / "big.bin") == t.contents["big.bin"]);
    CHECK(!std::filesystem::exists(out / "late.txt"));
}

// ---- writer::append ----

TEST_CASE("writer append: two-section archive, last-occurrence wins", "[reader][append]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w(arc);
        w.add_tree(t.root);
        w.finalize();
    }
    {
        auto w = writer::append(arc);
        // Re-add docs/readme.md with new content (versioning semantics) plus
        // a brand-new file.
        auto upd = td.path / "upd";
        write_file(upd / "docs" / "readme.md", make_content(777, 0xAA));
        write_file(upd / "new.txt", make_content(9, 0xBB));
        w.add_tree(upd);
        w.finalize();
    }

    reader r(arc);
    auto lst = r.list();
    std::size_t readme = 0, newtxt = 0;
    for (auto& le : lst) {
        if (le.path.generic_string() == "docs/readme.md") ++readme;
        if (le.path.generic_string() == "new.txt") ++newtxt;
    }
    CHECK(readme == 2);   // listed from both sections…
    CHECK(newtxt == 1);

    const auto out = td.path / "out";
    r.extract_all(out);
    // …but extraction uses the LATEST version (append = overwrite).
    CHECK(read_file(out / "docs/readme.md") == make_content(777, 0xAA));
    CHECK(read_file(out / "new.txt") == make_content(9, 0xBB));
    // old entries still intact
    CHECK(read_file(out / "big.bin") == t.contents["big.bin"]);
    CHECK(read_file(out / "docs/sub/notes.txt") == t.contents["docs/sub/notes.txt"]);
    // The hardlink pair split: readme.hard (canonical owner, section 1) keeps
    // the ORIGINAL content; readme.md now resolves to the section-2 version.
    CHECK(read_file(out / "docs/readme.hard") == t.contents["docs/readme.md"]);
    struct stat h{}, m{};
    REQUIRE(::stat((out / "docs/readme.hard").c_str(), &h) == 0);
    REQUIRE(::stat((out / "docs/readme.md").c_str(), &m) == 0);
    CHECK(h.st_ino != m.st_ino); // overwritten entry is no longer the same inode
}

TEST_CASE("writer append: empty append rewrites valid footer, sections kept",
          "[reader][append]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w(arc);
        w.add_tree(t.root);
        w.finalize();
    }
    std::uint64_t size_before = fsize(arc);
    {
        auto w = writer::append(arc);
        w.finalize(); // adds only an empty section
    }
    CHECK(fsize(arc) > size_before);
    reader r(arc);
    CHECK(r.list().size() >= t.contents.size() + 3);
    const auto out = td.path / "out";
    r.extract_all(out);
    CHECK(read_file(out / "big.bin") == t.contents["big.bin"]);
}

TEST_CASE("writer append: rejects a non-archive", "[append]") {
    temp_dir td;
    auto bogus = td.path / "bogus.arch";
    {
        std::ofstream os(bogus, std::ios::binary);
        os << std::string(272, 'q');
    }
    CHECK_THROWS_AS(writer::append(bogus), std::runtime_error);
    CHECK_THROWS_AS(writer::append(td.path / "missing.arch"), std::system_error);
}

TEST_CASE("writer append: footer DEDUP flag stays sticky", "[append][dedup]") {
    temp_dir td;
    auto arc = archive_in(td);
    {
        writer::options o;
        o.dedup = true;
        writer w(arc, o);
        auto d = td.path / "d1";
        write_file(d / "f", make_content(1024, 4));
        w.add_tree(d);
        w.finalize();
    }
    {
        writer::options o;
        o.dedup = false; // second section without dedup…
        auto w = writer::append(arc, o);
        auto d = td.path / "d2";
        write_file(d / "g", make_content(1024, 5));
        w.add_tree(d);
        w.finalize(); // …footer must still carry FOOTER_FLAG_DEDUP
    }
    file_io f(arc);
    auto fbytes = f.read(f.footer_off(), format::FOOTER_SIZE);
    auto foot = arch::detail::parse_footer(fbytes.data());
    CHECK(foot.flags.dedup());
    reader r(arc); // and everything still extracts
    const auto out = td.path / "out";
    r.extract_all(out);
    CHECK(read_file(out / "f") == make_content(1024, 4));
    CHECK(read_file(out / "g") == make_content(1024, 5));
}

// ---- dedup archives round-trip ----

TEST_CASE("reader extracts a dedup archive byte-identically", "[reader][dedup]") {
    temp_dir td;
    const auto blk = make_content(format::ARCH_BLOCK_SIZE, 0x5EED);
    std::vector<std::uint8_t> dup;
    dup.insert(dup.end(), blk.begin(), blk.end());
    dup.insert(dup.end(), blk.begin(), blk.end()); // same 4 MiB block twice
    dup.insert(dup.end(), blk.begin(), blk.end()); // …three times
    auto d = td.path / "src";
    write_file(d / "dup.bin", dup);

    auto arc = archive_in(td);
    std::uint64_t plain_size = 0;
    {
        writer::options o;
        o.dedup = false;
        writer w(archive_in(td, "plain.arch"), o);
        w.add_tree(d);
        w.finalize();
        plain_size = fsize(archive_in(td, "plain.arch"));
    }
    {
        writer::options o;
        o.dedup = true;
        writer w(arc, o);
        w.add_tree(d);
        w.finalize();
    }
    CHECK(fsize(arc) < plain_size); // dedup actually stored less

    reader r(arc);
    const auto out = td.path / "out";
    r.extract_all(out);
    CHECK(read_file(out / "dup.bin") == dup);
}

TEST_CASE("store mode: compressor=none roundtrips raw blocks on all paths",
          "[reader][none]") {
    temp_dir td;
    const auto payload = make_content(1u << 20, 0x51); // 1 MiB random-ish
    auto d = td.path / "src";
    write_file(d / "tree.bin", payload);
    write_file(d / "serial.bin", payload);

    auto arc = archive_in(td, "none.arch");
    {
        writer::options o;
        o.compressor = "none";
        writer w(arc, o);
        w.add_file(d / "serial.bin");                       // serial add_file
        w.add_file("mem.bin", std::string_view(               // in-memory path
            reinterpret_cast<const char*>(payload.data()), payload.size()));
        w.add_tree(d);                                      // pipeline path
        w.finalize();
    }
    { // threads = 0 → serial add_tree fallback, still "none"
        writer::options o;
        o.compressor = "none";
        o.threads = 0;
        auto w = writer::append(arc, o);
        w.add_tree(d);
        w.finalize();
    }

    { // Stored bytes must appear verbatim: no codec ran.
        file_io f(arc);
        auto data = f.read(0, f.size);
        std::vector<std::uint8_t> probe(payload.begin() + 4096,
                                        payload.begin() + 4096 + 256);
        auto it = std::search(data.begin(), data.end(), probe.begin(), probe.end());
        CHECK(it != data.end());
    }

    reader r(arc);
    const auto out = td.path / "out";
    r.extract_all(out);
    CHECK(read_file(out / "serial.bin") == payload);
    CHECK(read_file(out / "mem.bin") == payload);
    CHECK(read_file(out / "tree.bin") == payload);
}

// ---- pipelined writer feeds the reader ----

TEST_CASE("reader: pipelined multi-file archive extracts fully", "[reader][pipeline]") {
    temp_dir td;
    auto d = td.path / "many";
    std::map<std::string, std::vector<std::uint8_t>> want;
    for (int i = 0; i < 40; ++i) {
        const std::string name = "f" + std::to_string(i) + ".bin";
        want[name] = make_content(1024 * (i + 1) + 13, 0x1000 + i);
        write_file(d / name, want[name]);
    }
    auto arc = archive_in(td);
    {
        writer::options o;
        o.threads = 6;
        o.pipeline_depth = 8;
        writer w(arc, o);
        w.add_tree(d);
        w.finalize();
    }
    reader r(arc);
    const auto out = td.path / "out";
    r.extract_all(out);
    for (auto& [name, bytes] : want)
        CHECK(read_file(out / name) == bytes);
}

// ---- Phase 10: two-knob pool + profiling (docs/extract-pipeline.md §6) ----

TEST_CASE("reader extract: depth x threads matrix is byte-identical", "[reader][phase10]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w(arc);
        w.add_tree(t.root);
        w.finalize();
    }
    for (int depth : {1, 5, 32}) {
        for (int threads : {1, 4}) {
            const auto out = td.path / ("out_d" + std::to_string(depth) +
                                        "_t" + std::to_string(threads));
            reader::options ro;
            ro.depth = depth;
            ro.threads = threads;
            reader r(arc, ro);
            REQUIRE_NOTHROW(r.extract_all(out));
            for (auto& [rel, d] : t.contents)
                CHECK(read_file(out / rel) == d);
            // symlink + hardlink still materialised at every setting
            CHECK(fs::is_symlink(out / "docs/link.md"));
            CHECK(read_file(out / "docs/readme.hard") == t.contents["docs/readme.md"]);
        }
    }
}

TEST_CASE("reader profile report: contents and hints", "[reader][phase10][profile]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w(arc);
        w.add_tree(t.root);
        w.finalize();
    }
    // Off (default): empty report, nothing changes functionally.
    {
        reader r(arc);
        r.extract_all(td.path / "out0");
        CHECK(r.profile_report().empty());
    }
    // On: summary + at least the read/crc/write stages appear.
    reader::options ro;
    ro.profile = true;
    ro.depth = 2;
    reader r(arc, ro);
    const auto out = td.path / "outp";
    r.extract_all(out);
    for (auto& [rel, d] : t.contents)
        CHECK(read_file(out / rel) == d);
    const auto rep = r.profile_report();
    CHECK_FALSE(rep.empty());
    CHECK(rep.find("parch profile:") != std::string::npos);
    CHECK(rep.find("worker utilization") != std::string::npos);
    for (const char* stage : {"archive read", "crc32c", "decompress", "write out",
                              "create+open", "metadata"})
        CHECK(rep.find(stage) != std::string::npos);
    CHECK(rep.find("files") != std::string::npos); // summary line counts files
    // Counters are cumulative: verify on the same reader keeps growing them.
    CHECK(r.verify_all().empty());
    const auto rep2 = r.profile_report();
    auto ops_of = [](const std::string& s, const char* stage) {
        auto p = s.find(stage);
        REQUIRE(p != std::string::npos);
        auto q = s.find(" ops", p); // line reads "... (N ops, X us/op)"
        REQUIRE(q != std::string::npos);
        auto l = s.rfind('(', q);
        return std::stoull(s.substr(l + 1, q - l - 1));
    };
    CHECK(ops_of(rep2, "archive read") > ops_of(rep, "archive read"));
    // verify alone also reports (fresh reader, sink path exercises fd == -1)
    reader::options vo;
    vo.profile = true;
    reader r2(arc, vo);
    REQUIRE(r2.verify_all().empty());
    CHECK_FALSE(r2.profile_report().empty());
}

TEST_CASE("reader fd-based xattr/ACL restore lands on the extracted file", "[reader][phase10][xattr]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w(arc);
        w.add_tree(t.root);
        w.finalize();
    }
    const auto out = td.path / "out";
    reader r(arc);
    r.extract_all(out);
    char buf[64] = {};
    const ssize_t n = ::getxattr((out / "xattr.bin").c_str(), "user.arch.test",
                                 buf, sizeof buf);
    REQUIRE(n == 5);
    CHECK(std::string(buf, n) == "hello");
    // Mode survived too (fd-based fchmod path).
    struct stat st{};
    REQUIRE(::stat((out / "docs/readme.md").c_str(), &st) == 0);
    struct stat src{};
    REQUIRE(::stat((t.root / "docs/readme.md").c_str(), &src) == 0);
    CHECK((st.st_mode & 07777) == (src.st_mode & 07777));
}

TEST_CASE("reader verify_all: parallel error parity with serial depth=1", "[reader][phase10][verify]") {
    temp_dir td;
    auto t = make_tree(td.path);
    auto arc = archive_in(td);
    {
        writer w(arc);
        w.add_tree(t.root);
        w.finalize();
    }
    // Corrupt one data byte in the first block (data region starts right
    // after the 128-byte TOC header at end-of-section-0 layout offset).
    {
        file_io f(arc);
        f.flip(format::ARCHIVE_HEADER_SIZE + format::TOC_HEADER_SIZE + 10);
    }
    reader::options ro;
    ro.depth = 1;
    reader r1(arc, ro);
    auto serial = r1.verify_all();
    reader::options rp;
    rp.depth = 16;
    rp.threads = 4;
    reader r2(arc, rp);
    auto parallel = r2.verify_all();
    CHECK(serial == parallel);           // same errors, same order
    CHECK_FALSE(serial.empty());
}
