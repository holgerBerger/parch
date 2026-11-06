/*
 * archiver – archive format definitions
 *
 * All structures use explicit endianness helpers (be_*, le_*) for
 * on-disk storage so the format is identical on big- and little-endian hosts.
 * In-process types use native endianness.
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
constexpr std::size_t BLOCK_SIZE = 4ULL * 1024 * 1024; // 4 MiB
constexpr std::uint8_t COMPRESSOR_NONE = 0x00;
constexpr std::uint8_t COMPRESSOR_ZSTD  = 0x01;

// Magic bytes
inline constexpr std::string_view ARCH_MAGIC = "ARCH\x01\x00";
inline constexpr std::string_view TOC_MAGIC  = "TOC1";
inline constexpr std::string_view FOOTER_MAGIC = "AR01";

// ---- archive header (offset 0, 16 bytes) ----
struct archive_header {
    std::array<std::uint8_t, 16> magic_and_pad{}; // ARCH + ver + zeros
};

// ---- footer (256 bytes, always at EOF – 256) ----
struct footer_flags {
    std::uint16_t raw;
    bool dedup()    const { return raw & 0x01; }
    bool checked()  const { return raw & 0x02; }
};

struct footer {
    std::array<std::uint8_t, 4> magic{};  // "AR01"
    std::uint16_t version{};
    footer_flags flags{};
    std::uint64_t toc_count{};
    std::uint64_t toc_table_offset{};
    std::uint64_t total_uncompressed_size{};
    std::uint64_t total_compressed_size{};
    std::uint32_t toc_table_crc{};
    std::uint32_t footer_crc{};
    std::array<std::uint8_t, 204> reserved{};
    std::uint32_t signature_crc{};
};
static_assert(sizeof(footer) == 256);

// ---- TOC section reference ----
struct toc_section_ref {
    std::uint64_t offset{};     // absolute file offset of TOC header
    std::uint64_t size{};       // total size of this section
    std::uint32_t crc32c{};
    std::uint8_t reserved[4]{}; // pad to 20 → fits 64-bit alignment
};

// ---- TOC section header ----
struct toc_header {
    std::array<std::uint8_t, 4> magic{}; // "TOC1"
    std::uint64_t entry_count{};
    std::uint64_t data_offset{};         // relative to TOC header
    std::uint64_t data_size{};
    std::uint64_t string_table_offset{}; // relative
    std::uint64_t string_table_size{};
    std::uint64_t dedup_index_offset{};  // 0 if no dedup
    std::uint64_t dedup_index_size{};    // 0 if no dedup
    std::uint32_t crc32c{};
};

// ---- block entry (per compressed block) ----
struct block_entry {
    std::uint64_t offset;                 // bytes from TOC header to compressed data
    std::uint32_t compressed_size;
    std::uint32_t uncompressed_size;      // ≤ BLOCK_SIZE
    std::uint64_t content_id;             // xxHash3-64 (0 if no dedup)
    std::uint32_t crc32c;                // CRC of compressed data
    std::uint8_t  compressor_id;
    std::uint8_t  reserved[3];
};
static_assert(sizeof(block_entry) == 32);

// ---- file entry ----
enum class file_type : std::uint16_t {
    regular  = 0100000,
    symlink  = 0120000,
    directory = 0040000,
    blkdev   = 0060000,
    chrdev   = 0020000,
};

struct file_entry {
    std::uint64_t path_id;          // offset into string table
    std::uint16_t mode;             // POSIX mode bits (includes S_IFMT)
    std::uint32_t uid;
    std::uint32_t gid;
    std::uint64_t mtime_sec;
    std::uint32_t mtime_nsec;
    std::uint64_t atime_sec;
    std::uint32_t atime_nsec;
    std::uint64_t nlink;
    std::uint64_t uncompressed_size;
    std::uint64_t block_count;
    std::uint64_t block_list_offset; // into flat block_entry array
    std::uint64_t xattr_id;          // 0 if none
    std::uint64_t acl_id;            // 0 if none
    std::uint32_t checksum;          // CRC32C of uncompressed data
    std::uint32_t rdev_major;
    std::uint32_t rdev_minor;
};

// ---- dedup index entry (sorted by content_hash) ----
struct dedup_entry {
    std::uint64_t content_hash;
    std::uint64_t physical_offset;
    std::uint32_t reference_count;
    std::uint32_t reserved;
};

// ---- serialization helpers ----
// On-disk endian conversion
inline std::uint64_t htobe64_safe(std::uint64_t v) { return __builtin_bswap64(v); }
inline std::uint64_t betoh64_safe(std::uint64_t v) { return __builtin_bswap64(v); }
inline std::uint32_t htobe32_safe(std::uint32_t v) { return __builtin_bswap32(v); }
inline std::uint32_t betoh32_safe(std::uint32_t v) { return __builtin_bswap32(v); }
inline std::uint16_t htobe16_safe(std::uint16_t v) { return __builtin_bswap16(v); }
inline std::uint16_t betoh16_safe(std::uint16_t v) { return __builtin_bswap16(v); }

} // namespace arch::format
