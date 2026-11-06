# Archiver Format Specification

## Overview

A modern C++ archiver library optimized for network filesystems. Key design goals:

- **Large files**: Multi-threaded parallel compression per file (N threads split large file into chunks, compress concurrently)
- **Small files**: `io_uring`-based readahead prefetch to bridge network FS latency (a window of N files is prefetched while the compressor works on the current file)
- **Seek-based**: No streaming/pipe support — all archive operations use seeking
- **Append-friendly**: Multiple TOC sections, each self-contained
- **Integrity**: CRC32C on every compressed block + TOC section checksum
- **Dedup**: Optional block-level content-addressable storage (opt-in per archive)

## File Layout

```
Offset 0                                Offset N
+------------+------------------+------------------+-------+------+
|            |  TOC Section 0   |  TOC Section N   |  ... |Footer|
|            |------------------|------------------|       |      |
|            | entries         | entries          |       |      |
|            | + data blocks   | + data blocks    |       |      |
| Magic/Ver  | + string table  | + string table   |       |      |
| (16 bytes) |                |                   |       |      |
+------------+----------------+------------------ +-------+------+
```

**Creation flow:**
1. Write `ARCH` magic + version byte + reserved padding (16 bytes total) at offset 0
2. For each file: compute data, write compressed blocks, write file entry
3. After all files: write TOC section header (contains entry count, data block offset, sizes)
4. Write footer at end of file

**Append flow:**
1. Read footer, validate checksums
2. `lseek` to end of file
3. Write new data blocks + new TOC section
4. Rewrite footer (atomic via fsync + pwrite at known offset)

**Read flow:**
1. Read last ~256 bytes → parse footer → get TOC section count/locations
2. Seek to desired TOC section → parse entries → seek to referenced data blocks
3. Decompress → write to disk

## Footer (256 bytes, always at end of file)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 4 | magic `"AR01"` | Format identifier |
| 4 | 2 | version | Major:minor (0.1) |
| 6 | 2 | flags | Bitfield: `DEDUP=bit0`, `CHECKED=bit1` |
| 8 | 8 | toc_count | Number of TOC sections |
| 16 | 8 | toc_table_offset | Offset of TOC table (array of toc_section refs) |
| 24 | 8 | total_uncompressed_size | Sum of all file uncompressed sizes |
| 32 | 8 | total_compressed_size | Sum of all compressed block sizes |
| 40 | 8 | toc_table_crc | CRC32C of the TOC table data |
| 48 | 8 | footer_crc | CRC32C of bytes [48 .. 248) — self-check |
| 56 | 192 | reserved | Zero-filled, allows future expansion |
| 248 | 4 | signature CRC | CRC32C of bytes [0 .. 248) — footer integrity |

## TOC Table

An array immediately after the TOC table offset:

```c
struct toc_section_ref {
    uint64_t offset;       // File offset of TOC section header
    uint64_t size;         // Total size of this TOC section (header + entries + data + strings)
    uint32_t crc32c;       // CRC32C of entire TOC section
};
```

## TOC Section

Each TOC section is self-contained (all its data blocks live within the section).

### TOC Section Header

| Field | Type | Notes |
|-------|------|-------|
| magic | `uint32_t` | `"TOC1"` |
| entry_count | `uint64_t` | Number of file entries in this section |
| data_offset | `uint64_t` | Offset of first data block relative to TOC header |
| data_size | `uint64_t` | Total size of data block region |
| string_table_offset | `uint64_t` | Offset of string table relative to TOC header |
| string_table_size | `uint64_t` | Total size of string table |
| dedup_index_offset | `uint64_t` | 0 if no dedup, else offset of block hash index |
| dedup_index_size | `uint64_t` | 0 if no dedup, else size of block hash index |
| crc32c | `uint32_t` | CRC32C of entries + data + strings |

### File Entry

```c
struct file_entry {
    uint64_t path_id;           // Offset into string table → null-terminated path
    uint16_t mode;              // File mode (same as stat::st_mode)
    uint32_t uid;
    uint32_t gid;
    uint64_t mtime_sec;
    uint32_t mtime_nsec;
    uint64_t atime_sec;
    uint32_t atime_nsec;
    uint64_t nlink;
    uint64_t uncompressed_size;
    uint64_t block_count;       // Number of data blocks (0 for symlinks/dirs)
    uint64_t block_list_offset; // Offset into a flat block list array
    uint64_t xattr_id;          // Offset into xattr blob in string table, 0 if none
    uint64_t acl_id;            // Offset into ACL blob in string table, 0 if none
    uint32_t checksum;          // CRC32C of uncompressed file data
    uint32_t rdev_major;        // 0 for non-devices
    uint32_t rdev_minor;
};
```

### File Types

`mode & S_IFMT` determines entry type:
- `S_IFREG` — regular file (has data blocks)
- `S_IFLNK` — symbolic link (target in `path_str`)
- `S_IFDIR` — directory
- `S_IFBLK` / `S_IFCHR` — block/char device (uses `rdev_major/minor`)
- `S_IFIFO` / `S_IFSOCK` — stored as metadata-only (cannot archive actual sockets/streams)

### Block Entry (flat array, one per block across all files)

```c
struct block_entry {
    uint64_t offset;        // Offset of compressed data from TOC header
    uint32_t compressed_size;
    uint32_t uncompressed_size;  // Max BLOCK_SIZE (4MB), smaller for last block
    uint64_t content_id;    // 0 if no dedup, else 64-bit content hash (XXH3-64)
    uint32_t crc32c;        // CRC32C of the compressed data
    uint8_t  compressor_id;// Compressor magic byte (see Compressor Registry)
    uint8_t  reserved[3];
};
```

## Xattr Storage

Extended attributes are stored as a flat blob per file, pointed to by `xattr_id`:

```
[length (4 bytes BE), name_len (2 bytes), name[], value_len (4 bytes), value[]] × N
```

Terminated by `length == 0`.

## ACL Storage

Stored as POSIX.1e textual form (same as `getfacl` output), null-terminated string in the string table.
Separate access ACL and default ACL stored as concatenated null-terminated strings.

## Block Size

Fixed block size: **4 MiB** (0x400000). Last block of a file may be smaller.

A single uncompressed block is always exactly 4 MiB (except the last), which means:
- Compressed blocks are variable size (stored in `compressed_size`)
- Seek is trivial: block N starts at block array offset + N × block_entry_size, then follow `block_entry.offset`

## Compressor Registry

| ID | Name | Notes |
|----|------|-------|
| 0x00 | none | No compression (pass-through) |
| 0x01 | zstd | Zstandard, frame stored as-is |
| 0x10 | reserved | User-defined / future extensibility starts here |

The `compressor_id` is stored per-block (not globally) so different files can use different compressors.

## Deduplication

### How it works

When dedup is enabled (`DEDUP` flag set):

1. **Write time**: Before writing each uncompressed block, compute its 64-bit hash (`xxHash3-64`)
2. **Lookup**: Check the in-memory block hash index. If the hash already exists:
   - Do NOT write the block again
   - Set `content_id` to the existing hash
   - Mark the block as "shared"
3. **Store once**: If the hash is new, write the compressed block, record {hash → (toc_offset, data_offset)} in the index
4. **Read time**: When extracting, blocks with the same `content_id` share data — only decompress once, copy N times

### Block Hash Index (on-disk, per TOC section)

```c
struct dedup_entry {
    uint64_t content_hash;    // xxHash3-64 of uncompressed data
    uint64_t physical_offset; // Where the compressed block actually lives
    uint32_t reference_count; // How many block entries point here
    uint32_t reserved;
};
```

Sorted by `content_hash` for binary search during read.

### Dedup trade-offs

| Aspect | With dedup | Without dedup |
|--------|-----------|---------------|
| CPU | +1 × xxHash3 pass per block | - |
| RAM | Hash table (est. 64 bytes × unique blocks) | - |
| Write speed | Slightly slower (hash + hash table lookup) | Faster |
| Space | Saves on repeated content | No savings |
| Sequential read | Slightly slower (dedup index scan) | Faster |
| Random access | Same (blocks are individually addressable) | Same |

**Best for**: VM images, container layers, backup of filesystems with many identical files, any scenario with repeated content across files.

**Not worth it for**: Archives of unique files (photos, logs, database dumps).

## CRC/checksum scheme

Three levels:
1. **Block level**: CRC32C (Castagnoli) of compressed data in each block entry — verifies data integrity on read
2. **Entry level**: CRC32C of uncompressed file data in each file entry — verifies decompression correctness
3. **TOC level**: CRC32C of entire TOC section (entries + data + string table) — catches corruption/metadata damage
4. **Footer level**: CRC32C self-check + signature CRC — verifies footer was not truncated

CRC32C (`crc32c`) is preferred over CRC32 (Koopman) because hardware acceleration is available on x86 (`CRC32` instruction / `crc32.b` on ARM NEON).

## Magic bytes

| Location | Magic | Description |
|----------|-------|-------------|
| Offset 0 | `ARCH\x01\x00` + 10 zero bytes | Archive file header |
| Each TOC | `TOC1` | TOC section header |
| Footer | `AR01` | Footer (also at offset N-256) |
