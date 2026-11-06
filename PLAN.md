# Archiver — Project Plan & Status

## Overview

A C++20 archiver library optimized for network filesystems. The core idea: large
files get parallel compression (N threads per file), small files get io_uring
prefetch to bridge network FS latency. Existing archivers (tar, bsdtar) are too
slow on NFS/SMB because they read-and-compress sequentially through a single
stream.

**Key decision: custom format, not libarchive.** Libarchive's stream-oriented,
single-threaded API is fundamentally incompatible with parallel per-file
compression and async prefetch. See `docs/format.md` for the full spec.

## Design principles

- **Seek-based only** — no streaming/pipe support. All creation and extraction
  use seeking (`lseek`/`pwrite`/`pread`).
- **Append-friendly** — multiple self-contained TOC sections; appending means
  writing a new section at EOF and rewriting the footer.
- **Multi-thread per large file** — file split into 4 MiB blocks, each compressed
  concurrently by the compressor (zstd multithreading).
- **Prefetch for small files** — io_uring submit-read pipeline with a windowed
  read-ahead; while compressing block K, blocks K+1…K+N are already in flight.
- **Pluggable compressors** — `compressor` base class + factory registry;
  compressor ID stored per-block so different files can use different codecs.
- **Content dedup** — optional, per-TOC-section XXH3-64 hash of uncompressed
  blocks; duplicate blocks share physical data.
- **Integrity** — CRC32C (Castagnoli, hardware-accelerated) at block level, file
  entry level, TOC section level, and footer signature.

## Project status

| Area | Status |
|--|--|--|
| Format spec (`docs/format.md`) | ✅ Complete |
| Format structs (`include/arch/format.h`) | ✅ Complete — footer (256B), block_entry (32B), file_entry, toc_header, dedup_entry |
| Compressor interface (`include/arch/compressor.h`) | ✅ Complete — base class + factory + `REGISTER_COMPRESSOR` macro |
| zstd implementation | ✅ Complete — `ZSTD_compress2`/`ZSTD_decompress`, CRC32C, auto-registered |
| io_uring pool | ✅ Complete — SQE/CQE lifecycle, callback dispatch, read/write/statx/fsync/openat/close |
| Prefetcher | ⚠️ Scaffold — async read batches, ordered delivery via `pop_ready_chunk()`. `submit_prefetch_batch` has a raw-pointer ownership bug (line 59: `chunk.get()` on a newly-allocated chunk that isn't yet owned by `unique_ptr`). |
| Archive writer | ⚠️ Partial — header magic write, `add_file(path)` (single-threaded, pread loop), `add_file(path, data)` (in-memory), dedup logic. `finalize()` is incomplete (data_region/TOC/footer not fully serialized). `reader` methods are stubs. |
| Xattr util | ⚠️ Partial — `read_xattrs` via `listxattr`/`getxattr`, binary blob format written. `write_xattrs` and ACL functions are skeletons. |
| CMake (FetchContent) | ✅ Complete — spdlog, zstd, crc32c, xxHash via FetchContent. liburing via pkg-config. Catch2 v3.9.1. |
| Tests (Catch2) | ✅ 10 files, ~40 test cases — format sizes, compressor roundtrip, io_uring pool, prefetcher, writer smoke, CRC, endian, dedup index, footer serialize |
| CLI (`src/main.cpp`) | ❌ Missing — not created yet |
| Test plan (`docs/testing.md`) | ✅ Complete — 50+ planned future tests with priorities |

## Archive format (summary)

```
Offset 0                                              EOF
+-------------------+-------------------+---+--------+--+
| archive_header   |  TOC Section 0     | … | TOC N  |FO|
| (16 bytes, magic) + blocks + entries + strings + footer|
|                   |                   |   |        |256|
+-------------------+--------------------+---+--------+--+
```

**Footer (256 bytes at EOF):** magic `AR01`, version, flags (dedup bit), TOC count,
TOC table offset, total sizes, CRC32C fields, signature CRC.

**TOC section:** per-section header (`TOC1` magic), flat `file_entry[]` array, flat
`block_entry[]` array, string table (null-terminated paths), optional dedup index
(sorted by content hash).

**Block size:** 4 MiB fixed. Last block of a file may be smaller.

**Integrity levels:** block CRC32C (compressed data), file CRC32C (uncompressed),
TOC section CRC32C, footer signature CRC.

See `docs/format.md` for exact field layouts.

## File layout

```
include/
  arch/format.h            — on-disk struct definitions
  arch/compressor.h        — compressor base class + factory
  arch/zstd_impl.h         — zstd implementation
  arch/io_uring_pool.h     — io_uring submit/completion wrapper
  arch/prefetch.h          — read-ahead prefetcher
  arch/archive.h           — writer/reader high-level API
  util/xattr.h             — POSIX extended attributes
  util/acl.h               — POSIX ACLs (alias header)

src/
  archive/writer.cpp       — writer::impl (add_file, finalize, reader stubs)
  compress/compressor.cpp  — factory registry
  compress/zstd_impl.cpp   — zstd compress/decompress + CRC
  io/io_ring_pool.cpp      — io_uring SQE/CQE dispatch
  io/prefetch.cpp          — windowed async readahead
  util/xattr.cpp           — listxattr/getxattr/setxattr

test/
  test_format.cpp          — struct sizes, alignments
  test_compressor.cpp      — zstd roundtrip, factory, corruption
  test_io_uring.cpp        — pool write→read, fsync, multi-write
  test_prefetch.cpp        — ordered delivery, empty file, is_done
  test_archive_writer.cpp  — header magic, add_file smoke, options
  test_xattr.cpp           — xattr read roundtrip
  test_crc.cpp             — CRC32C and XXH3-64
  test_endian.cpp          — htobe*/betoh* roundtrip
  test_dedup_index.cpp     — dedup_entry sort + binary search
  test_footer_serialize.cpp— footer memcpy roundtrip

docs/
  format.md                — full archive format specification
  testing.md               — test plan with 50+ future tests
```

## Remaining work (ordered)

### Phase 1: Get it compiling & writer finishing
1. **Fix prefetcher bug** — `src/io/prefetch.cpp:48-72`: chunk is created with
   `new chunk()`, raw pointer captured before `unique_ptr` takes ownership.
   If an exception occurs between line 48 and 72, the chunk leaks. Switch to
   creating `unique_ptr` first, then using `chunk.get()` for the lambda.
2. **Create `src/main.cpp`** — CLI entry point: `arch create <archive> [files...]`,
   `arch extract <archive> [dest]`, `arch list <archive>`, `arch append <archive> [files...]`.
   Use spdlog for stdout progress.
3. **Wire up `src/archive/reader.cpp`** — currently lives inside writer.cpp as
   stubs. Needs its own file with proper footer parsing, TOC reading, block
   decompression, and file reconstruction.

### Phase 2: Serialization
4. **TOC serialization** — write `toc_header`, `file_entry[]`, `block_entry[]`,
   string table to disk in `finalize()`. Use big-endian encoding via
   `htobe` helpers. Compute CRC32C.
5. **Footer serialization** — after TOC is written, serialize footer struct,
   compute signature CRC, `pwrite` at known offset (filesize - 256).
6. **Reader deserialization** — `pread` footer, validate CRCs, read TOC table,
   iterate TOC sections, build in-memory entry list.

### Phase 3: Parallel compression
7. **Thread pool for block compression** — replace the sequential pread→compress
   loop in `add_file_impl` with: submit N blocks via pread (io_uring), each block
   gets compressed by a worker thread, completed blocks are written to archive in
   order.
8. **Integration with prefetcher** — for small files (< 16 MiB heuristic), use
   the prefetcher to feed the compressor. For large files (> 16 MiB), use the
   thread pool. Make the threshold configurable.

### Phase 4: Append support
9. **`writer::append()`** — open existing archive, `pread` footer, validate,
   seek to EOF, start writing new TOC section with its own data region and
   dedup index, add TOC section ref, rewrite footer atomically.

### Phase 5: Full xattr/ACL
10. **ACL support** — finish `util::read_acl` (`getfacl`), `util::write_acl`
    (`setfacl`). Wire into writer/reader.
11. **ACL roundtrip tests** — verify ACLs survive archive→extract cycle.

### Phase 6: Robustness
12. **Corruption detection** — reader verifies block CRC32C before decompressing,
    file CRC32C after decompression, TOC CRC32C on parse. Report which block/file
    failed.
13. **Sparse file support** — detect sparse files via `SEEK_DATA` fallback,
    only archive non-zero regions.
14. **Error recovery** — io_uring callback error handling, retry logic for
    transient network FS errors.

### Phase 7: Performance
15. **Benchmark suite** — measure throughput on local SSD, NFS (simulated latency),
    SMB. Compare against `tar --zstd` and `bsdtar -z`.
16. **Memory profiling** — per-thread zstd context reuse, prefetcher buffer pool,
    dedup hash table sizing.
17. **Dedup cross-section** — global dedup index that spans TOC sections (requires
    keeping the hash index between appends).

## Dependencies

| Dep | Origin | Version | Notes |
|--|--|--|--|
| spdlog | FetchContent (GitHub) | v1.15.3 | Logging. Transitively brings in fmt. |
| zstd | FetchContent (GitHub) | v1.5.7 | Static only, programs disabled. |
| crc32c | FetchContent (GitHub) | 1.1.2 | Hardware-accelerated CRC32C. |
| xxHash | FetchContent (GitHub) | v0.8.3 | XXH3-64 for dedup hashing. |
| liburing | pkg-config (system) | ≥ 2.9 | io_uring userspace lib. |
| Catch2 | FetchContent (GitHub) | v3.9.1 | Test framework. |

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

**Requires:** CMake ≥ 3.22, C++20 compiler, liburing development headers.

## Known issues / tech debt

- **`prefetch.cpp:59`** — `chunk.get()` called on raw-pointer `chunk` before
  `unique_ptr<chunk>` takes ownership. If the lambda captures `ptr` and an
  exception is thrown before line 72, memory is lost.
- **`io_uring_pool.cpp:153-160`** — callback lookup is O(N) linear search in
  `m_callbacks`. For high-throughput workloads, this vector should be replaced
  with a hash map keyed by tag.
- **`io_uring_pool.cpp:141`** — unused `head` variable.
- **`writer.cpp:69`** — magic header uses comma expressions (`hdr[0] = 'A',
  hdr[1] = 'R'`), should use `std::memcpy` or initializer list.
- **`writer.cpp:129`** — `std::filesystem::read_symlink` is not standard C++20,
  should use `readlink()` or `std::experimental::filesystem::symlink_status`.
- **`writer.cpp:284`** — string table `pwrite` at offset 0 overwrites the archive
  header. Needs proper offset calculation.
- **Test `test_compressor.cpp:65`** — references non-existent `zstd_impl(4)`
  constructor name `zstd_mt`, should just be `zstd_impl(4)`.
