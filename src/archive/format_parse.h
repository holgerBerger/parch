// Internal field-by-field big-endian parsers for the v0.2 on-disk layout
// (docs/format.md canonical offset tables). Mirror of the writer's
// serializers; shared by src/archive/reader.cpp and writer::append().
// Tests deliberately keep their own independent parsers (a symmetric bug in
// writer+reader must not hide).
#pragma once

#include "arch/format.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace arch::detail {

/// zip-slip guard (docs/format.md §String table): reject empty and absolute
/// member names and any `..` path element at extraction time.
inline void check_member_name(std::string_view name) {
    if (name.empty()) throw std::runtime_error("archive entry with empty path");
    if (name.front() == '/')
        throw std::runtime_error("absolute path in archive: " + std::string(name));
    std::size_t pos = 0;
    while (pos <= name.size()) {
        const auto slash = name.find('/', pos);
        const auto elem = name.substr(pos, slash == std::string_view::npos
                                                ? std::string_view::npos : slash - pos);
        if (elem == "..")
            throw std::runtime_error("zip-slip: '..' element in entry path: " +
                                     std::string(name));
        if (slash == std::string_view::npos) break;
        pos = slash + 1;
    }
}

class be_reader {
public:
    be_reader(const std::uint8_t* p, std::size_t n) : base_(p), p_(p), end_(p + n) {}

    std::uint16_t u16() { require(2); std::uint16_t v; std::memcpy(&v, p_, 2); p_ += 2; return format::betoh16_safe(v); }
    std::uint32_t u32() { require(4); std::uint32_t v; std::memcpy(&v, p_, 4); p_ += 4; return format::betoh32_safe(v); }
    std::uint64_t u64() { require(8); std::uint64_t v; std::memcpy(&v, p_, 8); p_ += 8; return format::betoh64_safe(v); }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    void raw(void* dst, std::size_t n) { require(n); std::memcpy(dst, p_, n); p_ += n; }
    std::size_t consumed() const { return static_cast<std::size_t>(p_ - base_); }

private:
    void require(std::size_t n) {
        if (static_cast<std::size_t>(end_ - p_) < n)
            throw std::runtime_error("archive truncated during metadata parse");
    }
    const std::uint8_t* base_;
    const std::uint8_t* p_;
    const std::uint8_t* end_;
};

inline format::footer parse_footer(const std::uint8_t* p /* 256 bytes */) {
    be_reader r(p, format::FOOTER_SIZE);
    format::footer f;
    r.raw(f.magic.data(), 4);                       // 0
    f.version = r.u16();                            // 4
    f.flags.raw = r.u16();                          // 6
    f.toc_count = r.u64();                          // 8
    f.toc_table_offset = r.u64();                   // 16
    f.total_uncompressed_size = r.u64();            // 24
    f.total_compressed_size = r.u64();              // 32
    f.toc_table_crc = r.u32();                      // 40
    f.footer_crc = r.u32();                         // 44
    r.raw(f.reserved.data(), 200);                  // 48
    f.signature_crc = r.u32();                      // 248
    f.reserved_tail = r.u32();                      // 252
    return f;
}

inline format::toc_section_ref parse_toc_ref(const std::uint8_t* p /* 24 bytes */) {
    be_reader r(p, format::TOC_SECTION_REF_SIZE);
    format::toc_section_ref t;
    t.offset = r.u64();          // 0
    t.size = r.u64();            // 8
    t.metadata_crc = r.u32();    // 16
    t.reserved = r.u32();        // 20
    return t;
}

inline format::toc_header parse_toc_header(const std::uint8_t* p /* 128 bytes */) {
    be_reader r(p, format::TOC_HEADER_SIZE);
    format::toc_header h;
    h.magic = r.u32();                       // 0
    h.entry_count = r.u64();                 // 4
    h.block_count = r.u64();                 // 12
    h.file_table_offset = r.u64();           // 20
    h.block_table_offset = r.u64();          // 28
    h.data_offset = r.u64();                 // 36
    h.data_size = r.u64();                   // 44
    h.string_table_offset = r.u64();         // 52
    h.string_table_size = r.u64();           // 60
    h.dedup_index_offset = r.u64();          // 68
    h.dedup_index_size = r.u64();            // 76
    std::uint8_t cid, res0;
    r.raw(&cid, 1);  h.compressor_id = cid;  // 84
    r.raw(&res0, 1); h.reserved0 = res0;     // 85
    h.compression_level = r.i16();           // 86
    h.section_flags = r.u32();               // 88
    h.metadata_crc = r.u32();                // 92
    r.raw(h.reserved.data(), 28);            // 96
    return h;
}

inline format::file_entry parse_file_entry(const std::uint8_t* p /* 128 bytes */) {
    be_reader r(p, format::FILE_ENTRY_SIZE);
    format::file_entry e;
    e.path_id = r.u64();                   // 0
    e.symlink_target_id = r.u64();         // 8
    e.link_target_id = r.u64();            // 16
    e.mode = r.u16();                      // 24
    e.uid = r.u32();                       // 26
    e.gid = r.u32();                       // 30
    e.mtime_sec = r.u64();                 // 34
    e.mtime_nsec = r.u32();                // 42
    e.atime_sec = r.u64();                 // 46
    e.atime_nsec = r.u32();                // 54
    e.nlink = r.u64();                     // 58
    e.uncompressed_size = r.u64();         // 66
    e.block_count = r.u64();               // 74
    e.block_list_offset = r.u64();         // 82
    e.xattr_id = r.u64();                  // 90
    e.acl_id = r.u64();                    // 98
    e.checksum = r.u32();                  // 106
    e.rdev_major = r.u32();                // 110
    e.rdev_minor = r.u32();                // 114
    r.raw(e.reserved_tail.data(), 10);     // 118
    return e;
}

inline format::block_entry parse_block_entry(const std::uint8_t* p /* 48 bytes */) {    be_reader r(p, format::BLOCK_ENTRY_SIZE);
    format::block_entry e;
    e.offset = r.u64();                        // 0
    e.compressed_size = r.u32();               // 8
    e.uncompressed_size = r.u32();             // 12
    r.raw(e.content_id.data(), 16);            // 16
    e.crc32c = r.u32();                        // 32
    std::uint8_t cid;
    r.raw(&cid, 1);
    e.compressor_id = cid;                     // 36
    r.raw(e.reserved.data(), 11);              // 37
    return e;
}

} // namespace arch::detail