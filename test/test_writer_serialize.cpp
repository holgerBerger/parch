// Phase 2: writer serialization conformance tests.
// These parse the archive *by spec* (docs/format.md v0.2 canonical offset
// tables) with their own big-endian readers — deliberately not reusing the
// writer's serializers, so a symmetric bug can't hide.
#include <catch2/catch_test_macros.hpp>
#include "arch/archive.h"
#include "arch/compressor.h"
#include "arch/format.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <crc32c/crc32c.h>

using namespace arch;

namespace {

struct arc_file {
    int fd = -1;
    explicit arc_file(const std::filesystem::path& p) {
        fd = ::open(p.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
    }
    ~arc_file() { if (fd >= 0) ::close(fd); }
    std::uint64_t size() const {
        struct stat st{};
        ::fstat(fd, &st);
        return st.st_size;
    }
    std::vector<std::uint8_t> read(std::uint64_t off, std::size_t n) const {
        std::vector<std::uint8_t> b(n);
        ssize_t got = 0;
        while (got < static_cast<ssize_t>(n)) {
            ssize_t r = ::pread(fd, b.data() + got, n - got, off + got);
            REQUIRE(r > 0);
            got += r;
        }
        return b;
    }
};

std::uint16_t be16(const std::uint8_t* p) {
    return (static_cast<std::uint16_t>(p[0]) << 8) | p[1];
}
std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(be16(p)) << 16) | be16(p + 2);
}
std::uint64_t be64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4);
}

struct parsed_entry {
    std::uint64_t path_id, symlink_target_id, link_target_id;
    std::uint16_t mode;
    std::uint64_t nlink, uncompressed_size, block_count, block_list_offset;
    std::uint64_t xattr_id, acl_id;
    std::uint32_t checksum;
    std::uint32_t rdev_major, rdev_minor;
};

parsed_entry parse_file_entry(const std::uint8_t* p) {
    parsed_entry e;
    e.path_id           = be64(p + 0);
    e.symlink_target_id = be64(p + 8);
    e.link_target_id    = be64(p + 16);
    e.mode              = be16(p + 24);
    e.uncompressed_size = be64(p + 66);
    e.block_count       = be64(p + 74);
    e.block_list_offset = be64(p + 82);
    e.xattr_id          = be64(p + 90);
    e.acl_id            = be64(p + 98);
    e.checksum          = be32(p + 106);
    e.rdev_major        = be32(p + 110);
    e.rdev_minor        = be32(p + 114);
    e.nlink             = be64(p + 58);
    return e;
}

struct parsed_block {
    std::uint64_t offset;
    std::uint32_t compressed_size, uncompressed_size, crc32c_val;
    std::array<std::uint8_t, 16> content_id;
    std::uint8_t compressor_id;
};

parsed_block parse_block_entry(const std::uint8_t* p) {
    parsed_block b;
    b.offset             = be64(p + 0);
    b.compressed_size    = be32(p + 8);
    b.uncompressed_size  = be32(p + 12);
    std::memcpy(b.content_id.data(), p + 16, 16);
    b.crc32c_val         = be32(p + 32);
    b.compressor_id      = p[36];
    return b;
}

} // namespace

TEST_CASE("writer produces spec-conformant archive (footer + TOC + CRCs)",
          "[writer][serialize]") {
    const auto arc = std::filesystem::temp_directory_path() / "arch_p2_basic.arc";
    std::string content = "The quick brown fox jumps over the lazy dog. ";
    for (int i = 0; i < 10; ++i) content += content.substr(0, 20); // ~400 B

    {
        arch::writer::options opts;
        opts.compressor = "zstd";
        opts.compression_level = 3;
        arch::writer w(arc, opts);
        w.add_file("hello.txt", content);
        w.finalize();
    }

    arc_file f(arc);
    const auto sz = f.size();
    REQUIRE(sz >= 16 + format::TOC_HEADER_SIZE + format::FOOTER_SIZE);

    // ---- archive header ----
    auto hdr = f.read(0, 16);
    REQUIRE(std::memcmp(hdr.data(), "ARCH\x01\x00", 6) == 0);

    // ---- footer at EOF-256 ----
    auto ft = f.read(sz - format::FOOTER_SIZE, format::FOOTER_SIZE);
    const std::uint8_t* fp = ft.data();
    REQUIRE(std::memcmp(fp, "AR01", 4) == 0);
    REQUIRE(be16(fp + 4) == 0x0002);                       // version 0.2
    const std::uint64_t toc_count = be64(fp + 8);
    const std::uint64_t table_off = be64(fp + 16);
    const std::uint64_t tot_unc   = be64(fp + 24);
    const std::uint64_t tot_cmp   = be64(fp + 32);
    REQUIRE(toc_count == 1);
    REQUIRE(tot_unc == content.size());

    // footer self-check: CRC over [0,44) ∪ [48,248)
    std::uint32_t fc = crc32c::Extend(0, fp, 44);
    fc = crc32c::Extend(fc, fp + 48, 248 - 48);
    REQUIRE(be32(fp + 44) == fc);
    // signature: CRC over [0,248)
    REQUIRE(be32(fp + 248) == crc32c::Crc32c(fp, 248));

    // ---- TOC table ----
    auto table = f.read(table_off, toc_count * format::TOC_SECTION_REF_SIZE);
    REQUIRE(be32(fp + 40) == crc32c::Crc32c(table.data(), table.size()));
    const std::uint64_t sec_off  = be64(table.data());
    const std::uint64_t sec_size = be64(table.data() + 8);
    const std::uint32_t sec_crc  = be32(table.data() + 16);
    REQUIRE(sec_off == 16);                                // first section after header
    REQUIRE(sec_off + sec_size == table_off);              // section ends where table begins

    // ---- TOC header ----
    auto th = f.read(sec_off, format::TOC_HEADER_SIZE);
    const std::uint8_t* hp = th.data();
    REQUIRE(std::memcmp(hp, "TOC1", 4) == 0);
    const std::uint64_t entry_count = be64(hp + 4);
    const std::uint64_t block_count = be64(hp + 12);
    const std::uint64_t ft_off = be64(hp + 20), bt_off = be64(hp + 28);
    const std::uint64_t data_off = be64(hp + 36), data_sz = be64(hp + 44);
    const std::uint64_t st_off = be64(hp + 52), st_sz = be64(hp + 60);
    const std::uint64_t di_off = be64(hp + 68), di_sz = be64(hp + 76);
    REQUIRE(entry_count == 1);
    REQUIRE(block_count >= 1);
    REQUIRE(data_off == format::TOC_HEADER_SIZE);
    REQUIRE(be16(hp + 86) == 3);                           // pinned level
    REQUIRE(hp[84] == format::COMPRESSOR_ZSTD);            // section codec
    REQUIRE(di_off == 0);                                      // dedup off
    REQUIRE(di_sz == 0);
    REQUIRE(ft_off + entry_count * format::FILE_ENTRY_SIZE == bt_off);
    REQUIRE(bt_off + block_count * format::BLOCK_ENTRY_SIZE == st_off);
    REQUIRE(st_off + st_sz == sec_size);                   // section is tight

    // metadata CRC: header (field zeroed) + file/block tables + strings.
    std::uint32_t mc = 0;
    auto hdr_copy = th;
    std::memset(hdr_copy.data() + 92, 0, 4);
    mc = crc32c::Extend(mc, hdr_copy.data(), hdr_copy.size());
    auto ftbl = f.read(sec_off + ft_off, entry_count * format::FILE_ENTRY_SIZE);
    mc = crc32c::Extend(mc, ftbl.data(), ftbl.size());
    auto btbl = f.read(sec_off + bt_off, block_count * format::BLOCK_ENTRY_SIZE);
    mc = crc32c::Extend(mc, btbl.data(), btbl.size());
    auto strs = f.read(sec_off + st_off, st_sz);
    mc = crc32c::Extend(mc, strs.data(), strs.size());
    REQUIRE(be32(hp + 92) == mc);
    REQUIRE(sec_crc == mc);

    // ---- file entry + string table ----
    auto e0 = parse_file_entry(ftbl.data());
    REQUIRE(e0.path_id < st_sz);
    REQUIRE(std::string(reinterpret_cast<const char*>(strs.data()) + e0.path_id)
            == "hello.txt");
    REQUIRE((e0.mode & S_IFMT) == S_IFREG);
    REQUIRE(e0.uncompressed_size == content.size());
    REQUIRE(e0.block_count == block_count);
    REQUIRE(e0.symlink_target_id == 0);
    REQUIRE(e0.link_target_id == 0);
    REQUIRE(e0.xattr_id == 0);
    REQUIRE(e0.checksum == crc32c::Crc32c(content.data(), content.size()));

    // ---- blocks: section-relative offsets, stored CRCs, reassembly ----
    std::string reassembled;
    std::uint32_t bcrc = 0;
    for (std::uint64_t i = 0; i < block_count; ++i) {
        auto b = parse_block_entry(btbl.data() + i * format::BLOCK_ENTRY_SIZE);
        REQUIRE(b.offset >= data_off);
        REQUIRE(b.offset + b.compressed_size <= sec_size);
        auto raw = f.read(sec_off + b.offset, b.compressed_size);
        REQUIRE(crc32c::Crc32c(raw.data(), raw.size()) == b.crc32c_val);
        REQUIRE(b.compressor_id == format::COMPRESSOR_ZSTD);
        bcrc = crc32c::Extend(bcrc, raw.data(), raw.size());
        auto z = compress::make_compressor("zstd", 3);
        auto plain = z->decompress(raw, b.uncompressed_size);
        REQUIRE(plain.size() == b.uncompressed_size);
        reassembled.append(reinterpret_cast<char*>(plain.data()), plain.size());
    }
    REQUIRE(reassembled == content);
    (void)bcrc; (void)tot_cmp;

    std::filesystem::remove(arc);
}

TEST_CASE("writer dedup section stores repeated block once", "[writer][serialize][dedup]") {
    const auto arc = std::filesystem::temp_directory_path() / "arch_p2_dedup.arc";
    // One full 4 MiB block duplicated: exactly two blocks, byte-aligned.
    std::string block(format::ARCH_BLOCK_SIZE, 'x');
    for (std::size_t i = 0; i < block.size(); i += 4096)
        block[i] = static_cast<char>((i / 4096) * 7 + 3);
    std::string content = block + block; // two identical blocks

    {
        arch::writer::options opts;
        opts.dedup = true;
        arch::writer w(arc, opts);
        w.add_file("dup.bin", content);
        w.finalize();
    }

    arc_file f(arc);
    const auto sz = f.size();
    auto ft = f.read(sz - format::FOOTER_SIZE, format::FOOTER_SIZE);
    REQUIRE(be16(ft.data() + 6) & format::FOOTER_FLAG_DEDUP);
    auto table = f.read(be64(ft.data() + 16), format::TOC_SECTION_REF_SIZE);
    auto th = f.read(be64(table.data()), format::TOC_HEADER_SIZE);
    const std::uint8_t* hp = th.data();
    const std::uint64_t block_count = be64(hp + 12);
    const std::uint64_t bt_off = be64(hp + 28);
    const std::uint64_t di_off = be64(hp + 68), di_sz = be64(hp + 76);
    const std::uint64_t data_sz = be64(hp + 44);
    REQUIRE(block_count == 2);              // two block entries...
    REQUIRE(di_sz == format::DEDUP_ENTRY_SIZE); // ... but one unique block
    auto btbl = f.read(be64(table.data()) + bt_off, block_count * format::BLOCK_ENTRY_SIZE);
    auto b0 = parse_block_entry(btbl.data());
    auto b1 = parse_block_entry(btbl.data() + format::BLOCK_ENTRY_SIZE);
    REQUIRE(b0.offset == b1.offset);            // same physical block
    REQUIRE(b0.crc32c_val == b1.crc32c_val);
    REQUIRE(b0.content_id == b1.content_id);
    REQUIRE(data_sz == b0.compressed_size);     // stored once
    auto dedup = f.read(be64(table.data()) + di_off, di_sz);
    // content_id non-zero (XXH3-128) and reference count 2.
    REQUIRE(std::memcmp(dedup.data(), b0.content_id.data(), 16) == 0);
    REQUIRE(be32(dedup.data() + 24) == 2);

    std::filesystem::remove(arc);
}

TEST_CASE("writer add_tree: dirs, symlinks, hardlinks, relative paths",
          "[writer][serialize][tree]") {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "arch_p2_tree";
    fs::remove_all(root);
    fs::create_directories(root / "sub");
    {
        std::ofstream(root / "a.txt") << "alpha contents";
        std::ofstream(root / "sub" / "b.txt") << "beta contents, longer than alpha\n";
    }
    fs::create_hard_link(root / "a.txt", root / "a_hard.txt");
    fs::create_symlink("a.txt", root / "link_to_a");

    const auto arc = root / ".." / "arch_p2_tree.arc";
    {
        arch::writer w(arc);
        w.add_tree(root);
        w.finalize();
    }

    arc_file f(arc);
    const auto sz = f.size();
    auto ft = f.read(sz - format::FOOTER_SIZE, format::FOOTER_SIZE);
    auto table = f.read(be64(ft.data() + 16), format::TOC_SECTION_REF_SIZE);
    const std::uint64_t sec = be64(table.data());
    auto th = f.read(sec, format::TOC_HEADER_SIZE);
    const std::uint8_t* hp = th.data();
    REQUIRE(std::memcmp(hp, "TOC1", 4) == 0);
    const std::uint64_t entry_count = be64(hp + 4);
    auto ftbl = f.read(sec + be64(hp + 20), entry_count * format::FILE_ENTRY_SIZE);
    auto strs = f.read(sec + be64(hp + 52), be64(hp + 60));
    auto str_at = [&](std::uint64_t id) {
        return std::string(reinterpret_cast<const char*>(strs.data()) + id);
    };

    // Expect entries: "a.txt", "a_hard.txt", "link_to_a", "sub", "sub/b.txt"
    std::map<std::string, parsed_entry> by_name;
    for (std::uint64_t i = 0; i < entry_count; ++i) {
        auto e = parse_file_entry(ftbl.data() + i * format::FILE_ENTRY_SIZE);
        by_name[str_at(e.path_id)] = e;
    }
    REQUIRE(by_name.size() == entry_count);
    REQUIRE(by_name.count("a.txt") == 1);        // relative paths only
    REQUIRE(by_name.count("sub/b.txt") == 1);
    REQUIRE(by_name.count(root.string()) == 0);  // root not stored

    // Symlink: target in string table via symlink_target_id, no blocks.
    const auto& lnk = by_name["link_to_a"];
    REQUIRE((lnk.mode & S_IFMT) == S_IFLNK);
    REQUIRE(lnk.block_count == 0);
    REQUIRE(lnk.symlink_target_id != 0);
    REQUIRE(str_at(lnk.symlink_target_id) == "a.txt");

    // Hardlink duplicate: references the first path via link_target_id, no blocks.
    const auto& hard = by_name["a_hard.txt"];
    const auto& owner = by_name["a.txt"];
    REQUIRE((hard.mode & S_IFMT) == S_IFREG);
    REQUIRE(hard.link_target_id != 0);
    REQUIRE(str_at(hard.link_target_id) == "a.txt"); // alphabetical-first owner
    REQUIRE(hard.block_count == 0);
    REQUIRE(owner.block_count >= 1);                 // owner carries the data

    // Directory entry exists with S_IFDIR.
    REQUIRE((by_name["sub"].mode & S_IFMT) == S_IFDIR);

    fs::remove_all(root);
    fs::remove(arc);
}

TEST_CASE("writer add_tree: tar-style store_prefix", "[writer][serialize][tree]") {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "arch_p2_prefix";
    fs::remove_all(root);
    fs::create_directories(root / "sub");
    {
        std::ofstream(root / "a.txt") << "alpha contents";
        std::ofstream(root / "sub" / "b.txt") << "beta contents, longer than alpha\n";
    }
    fs::create_hard_link(root / "a.txt", root / "a_hard.txt");
    fs::create_symlink("a.txt", root / "link_to_a");

    const auto arc = fs::temp_directory_path() / "arch_p2_prefix.arc";
    const auto arc_serial = fs::temp_directory_path() / "arch_p2_prefix_serial.arc";
    {
        arch::writer w(arc);
        w.add_tree(root, "proj");
        w.finalize();
    }

    arc_file f(arc);
    const auto sz = f.size();
    auto ft = f.read(sz - format::FOOTER_SIZE, format::FOOTER_SIZE);
    auto table = f.read(be64(ft.data() + 16), format::TOC_SECTION_REF_SIZE);
    const std::uint64_t sec = be64(table.data());
    auto th = f.read(sec, format::TOC_HEADER_SIZE);
    const std::uint8_t* hp = th.data();
    const std::uint64_t entry_count = be64(hp + 4);
    auto ftbl = f.read(sec + be64(hp + 20), entry_count * format::FILE_ENTRY_SIZE);
    auto strs = f.read(sec + be64(hp + 52), be64(hp + 60));
    auto str_at = [&](std::uint64_t id) {
        return std::string(reinterpret_cast<const char*>(strs.data()) + id);
    };

    std::map<std::string, parsed_entry> by_name;
    for (std::uint64_t i = 0; i < entry_count; ++i) {
        auto e = parse_file_entry(ftbl.data() + i * format::FILE_ENTRY_SIZE);
        by_name[str_at(e.path_id)] = e;
    }
    REQUIRE(by_name.size() == entry_count);
    // Everything moves under "proj/", and the root itself is now an entry —
    // stored FIRST (tar lists the top dir first).
    REQUIRE(str_at(parse_file_entry(ftbl.data()).path_id) == "proj");
    REQUIRE((by_name["proj"].mode & S_IFMT) == S_IFDIR);
    REQUIRE(by_name.count("proj/a.txt") == 1);
    REQUIRE(by_name.count("proj/sub/b.txt") == 1);
    REQUIRE(by_name.count("a.txt") == 0);
    // Hardlink owner reference carries the prefix; symlink target does not
    // (it is a raw link string).
    REQUIRE(str_at(by_name["proj/a_hard.txt"].link_target_id) == "proj/a.txt");
    REQUIRE(str_at(by_name["proj/link_to_a"].symlink_target_id) == "a.txt");

    // Pipelined and serial prefixed writes stay byte-identical.
    {
        arch::writer::options o;
        o.threads = 0;
        arch::writer w(arc_serial, o);
        w.add_tree(root, "proj");
        w.finalize();
    }
    const arc_file f_serial(arc_serial);
    CHECK(f.read(0, sz) == f_serial.read(0, f_serial.size()));

    // Bad prefixes rejected (format stores relative names only).
    {
        arch::writer w(fs::temp_directory_path() / "arch_p2_prefix_bad.arc");
        CHECK_THROWS_AS(w.add_tree(root, "/proj"), std::runtime_error);
        CHECK_THROWS_AS(w.add_tree(root, "../proj"), std::runtime_error);
        CHECK_THROWS_AS(w.add_tree(root, "a/./b"), std::runtime_error);
    }
    fs::remove(fs::temp_directory_path() / "arch_p2_prefix_bad.arc");

    // A multi-component prefix extracts fine even though intermediate dirs
    // ("top") are not entries themselves — extraction creates parents.
    const auto arc_nest = fs::temp_directory_path() / "arch_p2_prefix_nest.arc";
    {
        arch::writer w(arc_nest);
        w.add_tree(root, "top/proj/"); // trailing '/' is stripped
        w.finalize();
    }
    const auto dest = fs::temp_directory_path() / "arch_p2_prefix_dest";
    fs::remove_all(dest);
    arch::reader(arc_nest).extract_all(dest);
    CHECK(fs::exists(dest / "top/proj/a.txt"));
    CHECK(fs::exists(dest / "top/proj/sub/b.txt"));
    fs::remove_all(dest);
    fs::remove(arc_nest);

    fs::remove_all(root);
    fs::remove(arc);
    fs::remove(arc_serial);
}
