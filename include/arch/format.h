/*
 * archiver – archive format definitions (format spec v0.2, docs/format.md)
 *
 * IMPORTANT: the structs here are in-memory conveniences only. The
 * authoritative on-disk layout is the canonical big-endian offset tables in
 * docs/format.md; serialization is done FIELD BY FIELD via the htobe*_safe
 * helpers — these structs are never memcpy'd to or from disk, so their
 * padding is not part of the format. Record sizes on disk are the
 * *_SIZE constants below (static_asserts here guard natural padding choices
 * that keep sizeof == record size, catching accidental layout drift).
 *
 * All integer fields use native endianness in memory and are byte-swapped on
 * (de)serialization so the format is identical on big- and little-endian
 * hosts.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>
#include <optional>
#include <array>

namespace arch::format {

// ---- constants ----
// ARCH_ prefix: plain BLOCK_SIZE collides with the macro from <sys/mount.h>
// (pulled in transitively by <liburing.h>).
constexpr std::size_t ARCH_BLOCK_SIZE = 4ULL * 1024 * 1024; // 4 MiB
constexpr std::uint8_t COMPRESSOR_NONE = 0x00;
constexpr std::uint8_t COMPRESSOR_ZSTD  = 0x01;
// v0.3: sparse (hole) block — stores 0 bytes (compressed_size == 0),
// expands to uncompressed_size zero bytes at extraction. Not a codec:
// never passed to make_compressor_by_id.
constexpr std::uint8_t COMPRESSOR_SPARSE = 0x02;

// Format versions (footer::version). Archives stay 0x0002 unless a v0.3
// feature (sparse blocks) is actually used, then 0x0003.
constexpr std::uint16_t FORMAT_VERSION_BASE = 0x0002;
constexpr std::uint16_t FORMAT_VERSION_SPARSE = 0x0003;

// On-disk record sizes (docs/format.md v0.2)
constexpr std::size_t ARCHIVE_HEADER_SIZE = 16;
constexpr std::size_t FOOTER_SIZE         = 256;
constexpr std::size_t TOC_SECTION_REF_SIZE = 24;
constexpr std::size_t TOC_HEADER_SIZE      = 128;
constexpr std::size_t FILE_ENTRY_SIZE      = 128;
constexpr std::size_t BLOCK_ENTRY_SIZE     = 48;
constexpr std::size_t DEDUP_ENTRY_SIZE     = 32;

// Magic bytes
inline constexpr std::string_view ARCH_MAGIC = "ARCH\x01\x00";
inline constexpr std::string_view TOC_MAGIC  = "TOC1";
inline constexpr std::string_view FOOTER_MAGIC = "AR01";

// TOC section flags (toc_header::section_flags)
constexpr std::uint32_t SECTION_FLAG_DEDUP = 1u << 0;

// Footer flags (footer::flags raw bits)
constexpr std::uint16_t FOOTER_FLAG_DEDUP   = 1u << 0; // any section uses dedup
constexpr std::uint16_t FOOTER_FLAG_CHECKED = 1u << 1;

// 128-bit digest (XXH3-128 bytes; opaque — equality = match).
// All-zero means "no dedup / not computed".
using digest128 = std::array<std::uint8_t, 16>;

// ---- archive header (offset 0, 16 bytes) ----
struct archive_header {
    std::array<std::uint8_t, 16> magic_and_pad{}; // ARCH + ver + zeros
};
static_assert(sizeof(archive_header) == ARCHIVE_HEADER_SIZE);

// ---- footer (256 bytes, always at EOF – 256) ----
struct footer_flags {
    std::uint16_t raw;
    bool dedup()    const { return raw & FOOTER_FLAG_DEDUP; }
    bool checked()  const { return raw & FOOTER_FLAG_CHECKED; }
};

struct footer {
    std::array<std::uint8_t, 4> magic{};  // "AR01"
    std::uint16_t version{};              // 0x0002 for format v0.2
    footer_flags flags{0};
    std::uint64_t toc_count{};
    std::uint64_t toc_table_offset{};
    std::uint64_t total_uncompressed_size{};
    std::uint64_t total_compressed_size{};
    std::uint32_t toc_table_crc{};        // CRC32C of the TOC table array
    std::uint32_t footer_crc{};           // CRC32C of footer [0,44) + [48,248)
    std::array<std::uint8_t, 200> reserved{};
    std::uint32_t signature_crc{};        // CRC32C of footer [0,248)
    std::uint32_t reserved_tail{};
};
static_assert(sizeof(footer) == FOOTER_SIZE);

// ---- TOC section reference (TOC table entry, 24 bytes) ----
struct toc_section_ref {
    std::uint64_t offset{};         // absolute file offset of TOC header
    std::uint64_t size{};           // total size of this section
    std::uint32_t metadata_crc{};   // copy of toc_header::metadata_crc
    std::uint32_t reserved{};
};
static_assert(sizeof(toc_section_ref) == TOC_SECTION_REF_SIZE);

// ---- TOC section header (128 bytes) ----
struct toc_header {
    std::uint32_t magic{};              // "TOC1" as big-endian bytes
    std::uint64_t entry_count{};        // number of file_entry records
    std::uint64_t block_count{};        // number of block_entry records
    std::uint64_t file_table_offset{};  // section-relative
    std::uint64_t block_table_offset{}; // section-relative
    std::uint64_t data_offset{};        // section-relative offset of first block
    std::uint64_t data_size{};
    std::uint64_t string_table_offset{};// section-relative
    std::uint64_t string_table_size{};
    std::uint64_t dedup_index_offset{}; // 0 if no dedup
    std::uint64_t dedup_index_size{};   // 0 if no dedup
    std::uint8_t  compressor_id{};      // section default codec
    std::uint8_t  reserved0{};
    std::int16_t  compression_level{};  // pinned for the whole section
    std::uint32_t section_flags{};      // SECTION_FLAG_*
    std::uint32_t metadata_crc{};       // CRC32C over metadata only (see spec)
    std::array<std::uint8_t, 28> reserved{};
};
static_assert(sizeof(toc_header) == TOC_HEADER_SIZE);

// ---- block entry (per compressed block, 48 bytes) ----
struct block_entry {
    std::uint64_t offset;                 // section-relative bytes to compressed data
    std::uint32_t compressed_size;
    std::uint32_t uncompressed_size;      // ≤ ARCH_BLOCK_SIZE
    digest128     content_id{};           // XXH3-128 of uncompressed block (zero if no dedup)
    std::uint32_t crc32c;                 // CRC of compressed data
    std::uint8_t  compressor_id;
    std::array<std::uint8_t, 11> reserved{};
};
static_assert(sizeof(block_entry) == BLOCK_ENTRY_SIZE);

// ---- file entry (128 bytes) ----
enum class file_type : std::uint16_t {
    regular  = 0100000,
    symlink  = 0120000,
    directory = 0040000,
    blkdev   = 0060000,
    chrdev   = 0020000,
    fifo     = 0010000,
    sock     = 0140000,
};

struct file_entry {
    // Field declaration order differs from the canonical on-disk table in
    // docs/format.md (grouped u64 → u32 → u16) so the natural in-memory size
    // is exactly FILE_ENTRY_SIZE; serialization always follows the spec's
    // canonical offsets, never this layout.
    std::uint64_t path_id;            // string-table offset → relative path (NUL-terminated)
    std::uint64_t symlink_target_id;  // string-table offset → link target; 0 = not a symlink
    std::uint64_t link_target_id;     // string-table offset → first-archived path of shared
                                      // inode; 0 = not a hardlink duplicate
    std::uint64_t mtime_sec;
    std::uint64_t atime_sec;
    std::uint64_t nlink;
    std::uint64_t uncompressed_size;
    std::uint64_t block_count;
    std::uint64_t block_list_offset;  // index (not byte offset) into block_entry[]
    std::uint64_t xattr_id;           // 0 if none
    std::uint64_t acl_id;             // 0 if none
    std::uint32_t uid;
    std::uint32_t gid;
    std::uint32_t mtime_nsec;
    std::uint32_t atime_nsec;
    std::uint32_t checksum;           // CRC32C of uncompressed data
    std::uint32_t rdev_major;
    std::uint32_t rdev_minor;
    std::uint16_t mode;               // POSIX mode bits (includes S_IFMT)
    std::array<std::uint8_t, 10> reserved_tail{};
};
static_assert(sizeof(file_entry) == FILE_ENTRY_SIZE);

// ---- dedup index entry (sorted by content_hash, 32 bytes) ----
struct dedup_entry {
    digest128     content_hash{};     // XXH3-128 of uncompressed data
    std::uint64_t physical_offset;    // section-relative offset of stored block
    std::uint32_t reference_count;
    std::uint32_t reserved;
};
static_assert(sizeof(dedup_entry) == DEDUP_ENTRY_SIZE);

// ---- serialization helpers ----
// On-disk endian conversion. The format is big-endian on disk; these are
// no-ops on big-endian hosts and byte-swap on little-endian hosts.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
inline std::uint64_t htobe64_safe(std::uint64_t v) { return v; }
inline std::uint64_t betoh64_safe(std::uint64_t v) { return v; }
inline std::uint32_t htobe32_safe(std::uint32_t v) { return v; }
inline std::uint32_t betoh32_safe(std::uint32_t v) { return v; }
inline std::uint16_t htobe16_safe(std::uint16_t v) { return v; }
inline std::uint16_t betoh16_safe(std::uint16_t v) { return v; }
#else
inline std::uint64_t htobe64_safe(std::uint64_t v) { return __builtin_bswap64(v); }
inline std::uint64_t betoh64_safe(std::uint64_t v) { return __builtin_bswap64(v); }
inline std::uint32_t htobe32_safe(std::uint32_t v) { return __builtin_bswap32(v); }
inline std::uint32_t betoh32_safe(std::uint32_t v) { return __builtin_bswap32(v); }
inline std::uint16_t htobe16_safe(std::uint16_t v) { return __builtin_bswap16(v); }
inline std::uint16_t betoh16_safe(std::uint16_t v) { return __builtin_bswap16(v); }
#endif

} // namespace arch::format
