# Archiver Format Specification

**Version 0.2** (design freeze 2026-11 — the 0.1 sketch never produced a real
archive, so 0.2 is the first binding layout).

## Overview

A modern C++ archiver format optimized for network filesystems. Key design goals:

- **Large files**: blocks split into 4 MiB chunks and compressed concurrently by
  a shared block-compression thread pool.
- **Small files / many files**: a **file-level I/O pipeline** — a worker-thread
  pool stats/opens/reads **N files in flight simultaneously** (plain `pread`s,
  `statx` metadata), hiding NFS/SMB round-trip latency by concurrency *across*
  files rather than readahead within one. io_uring is an optional local-NVMe
  backend only (`ARCH_WITH_URING=ON`, default off).
- **Seek-based format, streamed writes**: random access via seeking; compressed
  blocks are written to disk as they are produced — only TOC metadata and the
  dedup index are buffered in RAM.
- **Append-friendly**: multiple **self-contained** TOC sections; each section
  owns its data, tables, and string references. Any section can be extracted or
  repaired independently of the others.
- **Integrity**: CRC32C on every compressed block, on uncompressed file data,
  on TOC *metadata* (not data — so `list()` never reads data blocks), and a
  footer self-check.
- **Dedup**: optional, **within-section** content-addressable storage keyed by a
  **128-bit XXH3** hash of uncompressed blocks (opt-in per section).

## Serialization rules (normative)

1. **Every structure is serialized field-by-field in big-endian byte order.**
   The C++ structs in `include/arch/format.h` are in-memory conveniences only —
   they are **never `memcpy`'d to or from disk**; struct padding is not part of
   the format. The offset tables below are the on-disk truth.
2. Integer fields are big-endian unless noted. Byte arrays (`content_id`,
   `content_hash`) are stored in the native byte order produced by XXH3 for a
   16-byte digest (treat as opaque 16 bytes; equality = match).
3. All sizes/offsets are in bytes. Section-relative offsets are relative to the
   start of that section's `toc_header`.
4. `0` is the sentinel for "absent" in every string-table reference field
   (`xattr_id`, `acl_id`, `symlink_target_id`, `link_target_id`). Byte 0 of the
   string table is therefore always a reserved `NUL` sentinel, never a real name.
5. Endianness helpers `htobe*_safe`/`betoh*_safe` dispatch on `__BYTE_ORDER__`
   (byte-swap on little-endian, identity on big-endian hosts).

## File Layout

```
Offset 0                                     Offset N
+-------------+----------------+-------+-----+-------------+--------+
|             | TOC Section 0  | ...   |TOC N|  TOC table  |Footer  |
|             | (header + data |       |     | (array of   |(256 B) |
| 16 B magic/ |  + entries +   |       |     |  toc_section|        |
| ver header  |  blocks +      |       |     |  _refs)     |        |
|             |  strings +     |       |     |             |        |
|             |  dedup index)  |       |     |             |        |
+-------------+----------------+-------+-----+-------------+--------+
```

**Creation flow (streamed):**
1. Write the 16-byte archive header magic at offset 0.
2. At the start of a TOC section, reserve 128 bytes for the `toc_header`.
3. For each file: stream compressed blocks straight to disk with `pwrite` at
   `section_start + data_offset` (append position advances as blocks land);
   buffer only `file_entry`/`block_entry`/string-table/dedup metadata in RAM.
4. After all files: append the file-entry array, block-entry array, string
   table, and (if enabled) the dedup index; compute the metadata CRC;
   `pwrite` the `toc_header` into the reserved slot at the section start.
5. Append the TOC table (array of `toc_section_ref`) and the footer.

**Append flow (durability contract):**
1. Read footer from EOF−256, validate CRCs, note old EOF position (discard any
   garbage past it).
2. Write the new TOC section (streamed as above), starting where the old footer
   began or after its end — new sections never rewrite old data.
3. Flush order, each step `fsync`-ed before the next:
   **data blocks → TOC section (header) → TOC table + new footer**.
4. The 256-byte footer write is a single small `pwrite` (one atomic RPC on NFS)
   at the new EOF−256.

**Crash-recovery rule:** a conforming reader treats any section or block
reference that extends past end-of-file as a **clean truncation point**: the
archive is valid up to the last fully-written section; a half-written trailing
section is dropped. No archive should be reported corrupt by this alone.

**Read flow:**
1. Read last 256 bytes → parse + validate footer → TOC count/locations.
2. Read the TOC table; per section, read `toc_header` + entry arrays + string
   table (**metadata only** — validating a `list()` never touches data blocks).
3. To extract: seek to referenced blocks, verify block CRC32C, decompress, write.

## Archive header (offset 0, 16 bytes)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 6 | magic | `"ARCH\x01\x00"` |
| 6 | 10 | reserved | Zero-filled |

The header is advisory. The **footer is authoritative** for format version and
validity; tools should trust it (fall back to scanning for a footer when the
header is damaged).

## Footer (256 bytes, always at end of file)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 4 | magic `"AR01"` | Format identifier |
| 4 | 2 | version | Major:minor (0.2) |
| 6 | 2 | flags | Bitfield: `DEDUP=bit0` (any section uses dedup), `CHECKED=bit1` |
| 8 | 8 | toc_count | Number of TOC sections |
| 16 | 8 | toc_table_offset | Offset of TOC table (array of `toc_section_ref`) |
| 24 | 8 | total_uncompressed_size | Sum of all file uncompressed sizes |
| 32 | 8 | total_compressed_size | Sum of all stored (dedup-aware) compressed bytes |
| 40 | 4 | toc_table_crc | CRC32C of the TOC table array |
| 44 | 4 | footer_crc | CRC32C of footer bytes [0, 44) ∪ [48, 248) |
| 48 | 200 | reserved | Zero-filled, future expansion |
| 248 | 4 | signature_crc | CRC32C of footer bytes [0, 248) |
| 252 | 4 | reserved | Zero |

## TOC table

An array of 24-byte records at `toc_table_offset`:

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 8 | offset | Absolute file offset of the section's `toc_header` |
| 8 | 8 | size | Total size of this TOC section |
| 16 | 4 | metadata_crc | Copy of the header's `metadata_crc` |
| 20 | 4 | reserved | Zero |

## TOC section

Self-contained: all data blocks, tables, and strings referenced by a section
live **inside that section**, and all its offsets are section-relative. Dedup
never crosses section boundaries (frozen decision).

Canonical element order inside a section (offsets are explicit in the header,
so future layouts can reorder):

```
toc_header (128 B, reserved at section start, patched at section completion)
data blocks (streamed as produced)
file_entry[]  (entry_count records, 128 B each)
block_entry[] (block_count records, 48 B each)
string table
dedup index   (optional, sorted by content_hash)
```

### TOC section header (128 bytes)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 4 | magic | `"TOC1"` |
| 4 | 8 | entry_count | Number of `file_entry` records |
| 12 | 8 | block_count | Number of `block_entry` records (this section) |
| 20 | 8 | file_table_offset | Section-relative offset of `file_entry[]` |
| 28 | 8 | block_table_offset | Section-relative offset of `block_entry[]` |
| 36 | 8 | data_offset | Section-relative offset of first data block |
| 44 | 8 | data_size | Byte length of the data region |
| 52 | 8 | string_table_offset | Section-relative |
| 60 | 8 | string_table_size | Bytes |
| 68 | 8 | dedup_index_offset | 0 if no dedup, else section-relative |
| 76 | 8 | dedup_index_size | 0 if no dedup, else bytes |
| 84 | 1 | compressor_id | Section default codec (see registry) |
| 85 | 1 | reserved | Zero |
| 86 | 2 | compression_level | Signed; **pinned for the whole section** |
| 88 | 4 | section_flags | `DEDUP=bit0` |
| 92 | 4 | metadata_crc | CRC32C over: this header with the field zeroed, then `file_entry[]`, `block_entry[]`, string table, dedup index. **Data blocks are NOT covered** (per-block CRCs are) — this is what makes `list()` cheap. |
| 96 | 32 | reserved | Zero-filled |

### File Entry (128 bytes, canonical layout)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 8 | path_id | String-table offset → null-terminated **relative** path |
| 8 | 8 | symlink_target_id | String-table offset → link target; 0 = not a symlink |
| 16 | 8 | link_target_id | String-table offset → first-archived path sharing this inode; 0 = not a hardlink duplicate. Extract re-creates via `link(target, path)`. |
| 24 | 2 | mode | Permission + type bits (`S_IFMT` subset fits in 16 bits on Linux) |
| 26 | 4 | uid | |
| 30 | 4 | gid | |
| 34 | 8 | mtime_sec | |
| 42 | 4 | mtime_nsec | |
| 46 | 8 | atime_sec | |
| 54 | 4 | atime_nsec | |
| 58 | 8 | nlink | Informational for regular files; authoritative for hardlink *duplicates* (which carry `link_target_id` instead) |
| 66 | 8 | uncompressed_size | |
| 74 | 8 | block_count | 0 for symlinks/dirs/devices |
| 82 | 8 | block_list_offset | **Index** (not byte offset) into this section's `block_entry[]` |
| 90 | 8 | xattr_id | String-table offset of xattr blob; 0 = none |
| 98 | 8 | acl_id | String-table offset of ACL text; 0 = none |
| 106 | 4 | checksum | CRC32C of the **uncompressed** file data |
| 110 | 4 | rdev_major | 0 for non-device files |
| 114 | 4 | rdev_minor | |
| 118 | 10 | reserved | Zero-filled |

### File types

`mode & S_IFMT` determines entry type:
- `S_IFREG` — regular file (has data blocks)
- `S_IFLNK` — symbolic link (**target stored via `symlink_target_id`** — the
  0.1 sketch's "target in path_str" was a bug: `path_id` is the entry's own name)
- `S_IFDIR` — directory
- `S_IFBLK` / `S_IFCHR` — device (uses `rdev_major/minor`)
- `S_IFIFO` / `S_IFSOCK` — metadata-only

### Block Entry (48 bytes, canonical layout)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 8 | offset | Section-relative offset of the compressed data (from this section's `toc_header`). **Never** crosses section boundaries — dedup duplicates point at an earlier `block_entry` in the *same* section. |
| 8 | 4 | compressed_size | Bytes stored on disk |
| 12 | 4 | uncompressed_size | ≤ `ARCH_BLOCK_SIZE` (4 MiB) |
| 16 | 16 | content_id | XXH3-**128** of uncompressed block; all-zero when dedup disabled. 128-bit (not 64) because a 64-bit collision silently substitutes the wrong block's data with no stored checksum able to notice. |
| 32 | 4 | crc32c | CRC32C of the compressed bytes |
| 36 | 1 | compressor_id | Codec for this block (see registry) |
| 37 | 11 | reserved | Zero-filled |

A duplicate block (dedup hit) stores the **same** `offset`, `compressed_size`,
`crc32c`, and `compressor_id` as the owning block entry — a reader can resolve
it from the `block_entry` alone, without consulting the dedup index.

### String table

Concatenated NUL-terminated UTF-8 byte strings: entry paths (relative),
symlink targets, ACL text, xattr blobs. Byte 0 is a reserved `NUL` sentinel so
that offset 0 always means "absent" in reference fields. Paths are stored
**relative to the `add_tree()` root**; absolute paths are invalid input for the
writer, and the reader must reject any name containing `..` path elements or a
leading `/` (zip-slip guard) at extraction.

## Xattr Storage

Extended attributes are stored as a blob in the string table, pointed to by
`xattr_id`:

```
[length (4 bytes BE), name_len (2 bytes BE), name[], value_len (4 bytes BE), value[]] × N
```

Terminated by `length == 0`.

## ACL Storage

Stored in the string table at `acl_id` as POSIX.1e textual form (`getfacl`
style, produced/consumed by libacl with **numeric IDs** so archives restore on
hosts lacking the source names). The access ACL and default ACL are stored as
concatenated NUL-terminated strings, positional (libacl text carries no
`default:` marker):

```
access_text NUL [default_text NUL] [NUL]     (writer appends the final NUL;
                                              the list ends at an empty string)
```

- Slot 0 is always the access ACL text (extended entries, `mask::` included).
  A mode-equivalent (trivial) access ACL is **not** stored at all — the blob
  is absent (`acl_id = 0`) and the mode bits reconstruct it. When only a
  default ACL exists, slot 0 holds a trivial base-3 placeholder derived from
  the mode so the default lands in slot 1.
- Slot 1 (directories only) is the default ACL text.

## Block Size

Fixed block size: **4 MiB** (0x400000) — `arch::format::ARCH_BLOCK_SIZE`
(note: renamed from `BLOCK_SIZE`, which collides with the `sys/mount.h` macro).
Last block of a file may be smaller.

- Compressed blocks are variable size (stored in `compressed_size`).
- Random access: locate the file's `block_list_offset` in `block_entry[]`,
  read record N, follow its section-relative `offset`.

## Compressor Registry

| ID | Name | Notes |
|----|------|-------|
| 0x00 | none | No compression (pass-through) |
| 0x01 | zstd | Zstandard, frame stored as-is |
| 0x10 | reserved | User-defined / future extensibility starts here |

The `compressor_id` is stored per-block so different files can use different
codecs; the **section default** (ID + level) lives in the `toc_header`.

**Determinism contract (normative):** a codec implementation registered for a
given ID **must** produce byte-identical output for identical input at the same
level. This is what makes a dedup hit verifiable: the duplicate's `crc32c` is
the owner's, and a reader that decompresses it and re-hashes gets the same
`content_id`. Codecs that cannot promise this must not be used with `dedup`.

## Deduplication

### How it works

When dedup is enabled (section flag `DEDUP`):

1. **Write time**: before compressing each block, compute its **XXH3-128** hash.
2. **Lookup**: check the in-memory hash index *for the current section*. If the
   hash exists: do NOT write the block again; copy the owner's
   (`offset`, `compressed_size`, `crc32c`, `compressor_id`) into the new
   `block_entry`, set `content_id`; bump `reference_count`.
3. **Store once**: if new, stream the compressed block, record
   {hash → (offset, ref_count)} in the index.
4. **Read time**: blocks sharing a `content_id` resolve to the same bytes;
   decompress once, fan out.
5. **Scope**: duplicates are only ever sought within the same section. After an
   append, identical content added in the new section is stored again (frozen
   trade-off: self-contained sections beat cross-append savings).

### Block Hash Index (on-disk, per TOC section, 32 bytes per record)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 16 | content_hash | XXH3-128 of uncompressed data |
| 16 | 8 | physical_offset | Section-relative offset of the stored block |
| 24 | 4 | reference_count | Number of block entries pointing here |
| 28 | 4 | reserved | Zero |

Sorted by `content_hash` (memcmp of the 16 bytes) for binary search during read.

### Dedup trade-offs

| Aspect | With dedup | Without dedup |
|--------|-----------|---------------|
| CPU | +1 × XXH3-128 pass per block | - |
| RAM | Hash table (~64 B per unique block; ≈16 GB per PiB of unique data at 4 MiB blocks) | - |
| Space | Saves on repeated, byte-aligned content | No savings |
| Random access | Same (blocks individually addressable) | Same |

**Best for**: VM images, container layers, filesystems with many identical
files. **Not worth it for**: archives of unique files (photos, logs, database
dumps). Fixed 4 MiB boundaries mean only byte-aligned repeats are caught — a
one-byte shift defeats cross-version dedup (content-defined chunking is out of
scope for v1).

## CRC/checksum scheme

Four levels:
1. **Block level**: CRC32C of the *compressed* bytes in each block entry —
   verifies data integrity on read.
2. **File level**: CRC32C of the *uncompressed* file data in each file entry —
   verifies decompression + assembly correctness end to end.
3. **TOC metadata level**: CRC32C of header + entry arrays + string table +
   dedup index — **not** the data region. `list()` validates cheaply.
4. **Footer level**: TOC-table CRC, footer self-check, signature CRC.

CRC32C (Castagnoli) is used because hardware acceleration is available on
x86 (`CRC32` instruction) and ARM (`crc32.b` NEON).

## Magic bytes

| Location | Magic | Description |
|----------|-------|-------------|
| Offset 0 | `ARCH\x01\x00` + 10 zero bytes | Archive file header (advisory) |
| Each TOC section | `TOC1` | Section header (offsets 0–3 of `toc_header`) |
| Footer (EOF−256) | `AR01` | Footer — authoritative |
