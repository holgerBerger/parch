# Test Plan

Catch2 v3 is the test framework. Tests live under `test/` and are wired into a single
`arch_test` binary via `catch_discover_tests()` so that `[tags]` on `TEST_CASE()`
drive individual CTest entries.

## Current tests (implemented)

| File | What it tests |
|--|-----|
| `test_format.cpp` | `block_entry`, `footer`, `dedup_entry` sizes and alignment; zero-init semantics; `file_entry` field defaults. |
| `test_compressor.cpp` | zstd compress/decompress roundtrip (8 KiB, 4 MiB, empty, incompressible random), factory create/reject, CRC32C != 0, corruption → empty result. |
| `test_io_uring.cpp` | `io_uring_pool` write→read roundtrip, fsync completion, 8 sequential writes all complete. |
| `test_prefetch.cpp` | `prefetcher` delivers chunks in offset order, empty-file handling, `is_done()` after full consume. |
| `test_archive_writer.cpp` | writer creates archive with magic header, in-memory file add, real-file add, default options, invalid-path rejection; reader throws on missing file. |
| `test_xattr.cpp` | `util::read_xattrs` roundtrip: read xattr blob from temp file, file-without-xattrs returns 0. |
| `test_crc.cpp` | CRC32C empty = 0, known-string value, incremental vs. single-update equivalence; XXH3-64 determinism + distinctness. |
| `test_endian.cpp` | `htobe*/betoh*` roundtrip for 8/4/2-byte values, zero preservation. |
| `test_dedup_index.cpp` | sorted `dedup_entry` vector + binary-search lookup, empty-case, struct size. |
| `test_footer_serialize.cpp` | footer all-zero defaults, memcpy roundtrip of every field. |

## Future tests (to implement as code is written)

Grouped by subsystem. Priority order is indicated by `P1` (critical), `P2` (important), `P3` (nice).

### Format / Serialization

| # | Test | Tag | Priority |
|---|------|-----|----------|
| F1 | `toc_header` roundtrip — serialize via `pwrite()` with big-endian encoding, read back via `pread()`, verify all fields | `[format][toc]` | P1 |
| F2 | `file_entry` on-disk roundtrip — serialize, re-read, verify path_id, mode, uid/gid, mtime, block_count, checksum | `[format][entry]` | P1 |
| F3 | `block_entry` on-disk roundtrip — serialize with non-zero CRC32C, re-read and check all fields | `[format][block]` | P1 |
| F4 | `toc_section_ref` roundtrip — write → read array, verify offset/size/crc | `[format][toc]` | P1 |
| F5 | Footer CRC32C self-check — craft valid footer, mutate field, verify signature CRC mismatch detection | `[format][footer][integrity]` | P1 |
| F6 | Mixed-endian format — serialize on host, bit-flip endianness by hand, verify `betoh*` recovers correctly | `[format][endian]` | P2 |
| F7 | String table serialization — multiple paths with embedded NULs in parent, verify offset lookup | `[format][stringtable]` | P2 |

### Compression

| # | Test | Tag | Priority |
|---|------|-----|----------|
| C1 | lz4 compressor (when added) — roundtrip, id byte 0x10, factory registration | `[compressor][lz4]` | P2 |
| C2 | no-op compressor (id 0x00) — pass-through with no size reduction | `[compressor][noop]` | P2 |
| C3 | Compressor factory concurrent access — `make_compressor()` from N threads simultaneously | `[compressor][concurrent]` | P2 |
| C4 | zstd at different levels (1–22) — verify all produce valid roundtrip | `[compressor][zstd][slow]` | P3 |
| C5 | Compressed block CRC mismatch injection — corrupt compressed data by 1 byte, verify CRC32C check fails | `[compressor][integrity]` | P1 |

### I/O (io_uring)

| # | Test | Tag | Priority |
|---|------|-----|----------|
| I1 | `io_uring_pool` submit_read on closed fd — callback receives negative errno | `[io_uring][error]` | P1 |
| I2 | `io_uring_pool` submit_write exceeds file size — partial write detection | `[io_uring][error]` | P2 |
| I3 | Concurrent SQE submissions (1024 at once) — all completions fire, no lost callbacks | `[io_uring][concurrent]` | P1 |
| I4 | `submit_openat` + `submit_close` roundtrip | `[io_uring]` | P2 |
| I5 | `submit_statx` for existing vs. missing file | `[io_uring]` | P2 |
| I6 | Callback exception safety — callback throws, other callbacks still fire | `[io_uring][error]` | P1 |

### Prefetch

| # | Test | Tag | Priority |
|---|------|-----|----------|
| P1 | Large file prefetch (64 MiB, 64 KiB chunks) — all 1024 chunks delivered in order | `[prefetch][slow]` | P1 |
| P2 | Small-file many-batches — 10 files, each ~100 KiB, prefetcher reused | `[prefetch]` | P1 |
| P3 | Prefetch window smaller than file — verify read-ahead boundary | `[prefetch]` | P2 |
| P4 | Prefetcher on file with holes (sparse file) — handles partial reads gracefully | `[prefetch][sparse]` | P2 |
| P5 | Timeout / cancellation — destroy prefetcher mid-flight, no crash or leak | `[prefetch][lifecycle]` | P1 |

### Archive writer

| # | Test | Tag | Priority |
|---|------|-----|----------|
| W1 | Write multi-file archive — 5 files, verify footer exists + header magic | `[writer]` | P1 |
| W2 | Write directory entry (no data blocks) — `block_count == 0` | `[writer]` | P1 |
| W3 | Write symlink entry — `S_IFLNK`, target stored in path, `block_count == 0` | `[writer]` | P1 |
| W4 | Write device entry — `rdev_major/minor` populated | `[writer]` | P2 |
| W5 | Large file split into multiple blocks — verify `block_count == ceil(size/4MiB)` | `[writer][slow]` | P1 |
| W6 | Append to existing archive — write 3 files, finalize, then append 2 more, verify both TOC sections | `[writer][append]` | P1 |
| W7 | Dedup archive — two identical files, verify only one data block written, both entries share `content_id` | `[writer][dedup]` | P1 |
| W8 | Xattr roundtrip — `add_file` on file with xattr, verify blob in archive, re-read on extract | `[writer][xattr]` | P1 |
| W9 | ACL roundtrip — set ACL on file, verify `getfacl`-style blob stored | `[writer][acl]` | P2 |
| W10 | Writer with custom options — `threads=8`, `dedup=true`, `prefsetch_window=32` | `[writer]` | P2 |
| W11 | Error handling — write to read-only filesystem (if testinfra allows), verify graceful error | `[writer][error]` | P2 |
| W12 | Writer fsync before close — verify `fsync()` called on finalize | `[writer][integrity]` | P2 |

### Archive reader (when `reader::impl` fleshed out)

| # | Test | Tag | Priority |
|---|------|-----|----------|
| R1 | List entries from multi-file archive — verify count and paths | `[reader]` | P1 |
| R2 | Extract single file — verify content matches original byte-for-byte | `[reader]` | P1 |
| R3 | Extract all files — verify every file in archive, including empty files | `[reader][slow]` | P1 |
| R4 | Read xattrs from archive entry — `get_xattrs()` returns expected key/value | `[reader][xattr]` | P1 |
| R5 | Read ACL from archive entry — `get_acl()` returns text | `[reader][acl]` | P2 |
| R6 | Extract with block-level CRC check — inject 1-byte corruption in a block entry, verify decompath fails | `[reader][integrity]` | P1 |
| R7 | Extract with entry-level CRC check — verify uncompressed data CRC in `file_entry` | `[reader][integrity]` | P1 |
| R8 | Multi-TOC archive — extract file from TOC section 0, then section 1 (appended archive) | `[reader][append]` | P1 |
| R9 | Dedup archive extraction — verify N references to same block produce identical files | `[reader][dedup]` | P1 |
| R10 | Reader corruption tolerance — truncate 100 bytes from middle of data region, verify partial recoverability | `[reader][corruption]` | P2 |
| R11 | Reader handles mixed compressors — some blocks zstd, some uncompressed, in same archive | `[reader]` | P2 |

### Network filesystem simulation (integration-level)

| # | Test | Tag | Priority |
|---|------|-----|----------|
| N1 | Archiver on fuse-overlay with artificial latency — 10ms per read, verify prefetch window absorbs it | `[integration][slow]` | P2 |
| N2 | Many small files on tmpfs vs. network fs proxy — benchmark baseline throughput | `[integration][benchmark][slow]` | P3 |
| N3 | Large file parallel compression throughput — 1 GiB file, N threads, measure wall-clock vs. single-thread | `[integration][benchmark][slow]` | P3 |
| N4 | Mixed workload — 1000 small files + 10 large files, verify no OOM, all extracted correctly | `[integration][slow]` | P2 |

### Util

| # | Test | Tag | Priority |
|---|------|-----|----------|
| U1 | `util::write_xattrs` roundtrip — serialize blob, write to file, read back with `getxattr()` | `[util][xattr]` | P1 |
| U2 | `util::write_xattrs` on file with existing xattrs — verify overwrite | `[util][xattr]` | P2 |
| U3 | `util::read_acl` / `write_acl` — libacl roundtrip (implemented Phase 7) | `[util][acl]` | P2 |

## Test tags / filter strategy

```
ctest -R '^\[fast\]'     # everything except [slow]
ctest -R '^\[slow\]'     # slow/benchmark tests (CI: optional)
ctest -R '^\[compressor\]'  # just compressor subsystem
ctest -R '^\[integrity\]'   # all data-integrity tests
```

The `[slow]` tag should be added to any test that:
- Allocates more than 64 MiB
- Takes more than 5 seconds
- Requires a network filesystem or fuse layer

## Coverage goals

| Layer | Target |
|--|------|
| Format structs (serialization) | 100% |
| Compressor interface | 100% |
| io_uring pool | ≥ 90% |
| Prefetcher | ≥ 85% |
| Archive writer | ≥ 80% |
| Archive reader | ≥ 80% |
| Util (xattr/acl) | ≥ 70% |
