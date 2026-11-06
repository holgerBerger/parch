# Archiver — Project Plan & Status

## Overview

A C++20 archiver library optimized for network filesystems. The core idea: a
**file-level pipeline** — stat/open/read of many files in flight concurrently via
a pread worker-thread pool, feeding a block-compression pool that keeps cores fed.
This bridges network-FS latency in *both* directions (across files, not within one
file). Existing archivers (tar, bsdtar) are too slow on NFS/SMB because they
read-and-compress sequentially through a single stream. io_uring is demoted to an
optional local-NVMe backend (`ARCH_WITH_URING`, default off).

**Key decision: custom format, not libarchive.** Libarchive's stream-oriented,
single-threaded API is fundamentally incompatible with parallel per-file
compression and random per-file access. See `docs/format.md` for the full spec
(a revision pass lands in Phase 1 of the plan below).

## Design principles

- **Seek-based archive, streamed writes** — no streaming/pipe *format* support;
  all operations use seeking (`lseek`/`pwrite`/`pread`). But compressed blocks are
  written to disk **as they are produced** — only TOC metadata and the dedup
  index are buffered in RAM. Memory use never scales with archive size.
- **Append-friendly, self-contained sections** — multiple TOC sections, each
  holding its own entries, data blocks and string table, so any section can be
  extracted/repaired independently. Dedup is deliberately **within-section only**
  (a frozen decision — cross-section refs would break self-containment).
- **Pipeline across files, not within one** — the network-FS win is concurrency
  over *files*: a worker pool stats/opens/reads N files simultaneously (plain
  `pread`s — no special kernel API needed, works on any FS) while a separate
  thread pool compresses ready blocks from all in-flight files. io_uring remains
  an optional backend for local NVMe (`ARCH_WITH_URING=ON`).
- **Parallel compression, one knob** — files are split into 4 MiB blocks;
  `options.threads` sizes the block-compression pool (parallelism across *and*
  within files). zstd runs `nbWorkers=1` per block — the pool *is* the knob,
  no double-counting.
- **Pluggable compressors** — `compressor` base class + factory registry;
  compressor ID stored per-block. The codec *and compression level* are pinned
  per TOC section (recorded in the TOC header) so "same input + same codec +
  same level ⇒ identical bytes" holds archive-wide.
- **Content dedup** — optional, per-TOC-section **XXH3-128** hash of uncompressed
  blocks (`block_entry.content_id` grows to 16 B, block_entry 32→48 B); duplicate
  blocks share physical data. 128-bit identity removes the silent-collision
  corruption risk a 64-bit hash carries.
- **Full POSIX fidelity** — symlink targets are stored (string-table reference),
  hardlinks are real (`link_target` field + inode map during tree walk),
  xattrs/ACLs per spec. Paths are stored **relative** to the `add_tree()` root;
  extraction rejects absolute or `..`-escaping names (zip-slip).
- **Integrity** — CRC32C (Castagnoli, hardware-accelerated) on compressed blocks,
  on uncompressed file data, on TOC **metadata only** (header + entries + strings
  — so `list()` never touches data blocks), plus footer self-check.
- **Durable append** — write order data → fsync → TOC → fsync → TOC table +
  footer → fsync; the reader treats any reference extending past EOF as a clean
  truncation point (the crash-recovery contract, since a footer rewrite is not
  atomic on NFS).

## Project status

| Area | Status |
|--|--|--|
| Format spec (`docs/format.md`) | ✅ **Revised to v0.2** (2026-11 freeze): canonical per-field offset tables, 48 B `block_entry` with XXH3-128 `content_id`, 128 B `file_entry` with `symlink_target_id`/`link_target_id`, metadata-only TOC CRC, self-contained sections, durability + zip-slip contracts. `docs/api.md` updated to match. |
| Format structs (`include/arch/format.h`) | ✅ **Ported to v0.2** (Phase 1 code half done): `ARCH_BLOCK_SIZE` rename, 48 B `block_entry` with `digest128 content_id`, 128 B `file_entry` with `symlink_target_id`/`link_target_id`, 128 B `toc_header` (codec + level + section_flags + metadata_crc), 32 B `dedup_entry`, endianness dispatch via `__BYTE_ORDER__`; canonical `*_SIZE` constants + `static_assert`s; size tests updated. Serializer (field-by-field writes) is Phase 2. |
| Compressor interface (`include/arch/compressor.h`) | ✅ Complete — base class + level-taking factory (`make_compressor(name, level)`, spec v0.2) + `REGISTER_COMPRESSOR` macro |
| zstd implementation | ✅ Compiles & passes roundtrip tests — header self-contained; real APIs (`ZSTD_c_nbWorkers=1`, `ZSTD_c_compressionLevel`, `crc32c::Crc32c/Extend`); level is the constructor arg (pinned per section in Phase 2) |
| io_uring pool | ✅ **Optional backend** (`ARCH_WITH_URING=ON`, default OFF) compiles, tests pass (46/46, 100× stress, ASan/UBSan-clean). Fixed: `<liburing.h>`, `next_tag` as member, removed `io_uring_queue_quiesce`, **`io_uring_queue_init` misuse (CQ depth passed as FLAGS ⇒ `IORING_SETUP_R_DISABLED` ring that never completed anything)**, **`drain_and_stop` deadlock** (timed waits + pending-op counter + condvar), and **a CQE-consumption race** (liburing completion reaping is not thread-safe; workers had double-consumed CQEs ⇒ callbacks fired twice ⇒ use-after-free — now serialized under a dedicated reaping mutex, callbacks dispatched outside it). Callback registry is now `unordered_map` (former O(N) perf item, done). |
| Prefetcher | ✅ **To be superseded** by the thread-pool file pipeline (Phase 3); this io_uring variant stays as the optional backend. Fixed the compile error (raw `new` + `.get()`), the `delete`+`erase` double-free, and a **permanent block in `pop_ready_chunk`** (`m_eof` was only set on a branch that non-empty files never reached). Chunk ownership rewritten to `shared_ptr` held by the io callback itself — the kernel read buffer and completion flag can no longer be freed underneath the worker (was a use-after-free). |
| Archive writer | ⚠️ Partial — compiles & tests green; `add_file_impl` is now a member (pimpl access fixed), XXH3-**128** dedup hashing wired, dedup hits copy the owner's `compressed_size`/`crc32c`/`compressor_id`, string-table `pwrite` moved off offset 0. `finalize()` still buffers everything in RAM and the TOC section/footer serialization is a stub — Phase 2. `reader` methods are stubs — Phase 4. |
| Xattr util | ⚠️ Partial — `read_xattrs` via `listxattr`/`getxattr`, binary blob format written. `write_xattrs` and ACL functions are skeletons. Compiles. |
| CMake (FetchContent) | ✅ Configures & builds from scratch: policy-version workaround for crc32c, Catch2 `extras/` module path + `include(Catch)`, `spdlog::spdlog` target, `SOURCE_SUBDIR` for zstd (`build/cmake`) and xxHash (`cmake_unofficial`), `option(ARCH_WITH_URING OFF)` gating pool/prefetch sources + tests. |
| Tests (Catch2) | ✅ **40/40 passing** (default) / **46/46** (`ARCH_WITH_URING=ON`) — `Catch2WithMain` linked, `test_toc_serialize.cpp` created, `zstd_mt`→`zstd_impl`, crc32c/`make_compressor` API usages real, io_uring tests gated on the flag. |
| CLI (`src/main.cpp`) | ⚠️ Stub exists (usage banner; commands land in Phase 5) so the `arch` target builds |
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
`block_entry[]` array, string table (null-terminated paths + symlink targets +
xattr/ACL blobs), optional dedup index (sorted by content hash). Self-contained:
all offsets are relative to the section header; dedup never crosses sections.

**Block size:** 4 MiB fixed. Last block of a file may be smaller.

**Integrity levels:** block CRC32C (compressed data), file CRC32C (uncompressed),
TOC **metadata** CRC32C (header + entries + strings only — `list()` never reads
data blocks), footer self-check + signature CRC.

See `docs/format.md` for exact field layouts.

## File layout

```
include/
  arch/format.h            — on-disk struct definitions (in-memory helpers over canonical layout)
  arch/compressor.h        — compressor base class + factory
  arch/zstd_impl.h         — zstd implementation
  arch/io.h                — file-I/O pipeline interface (planned, Phase 3)
  arch/io_uring_pool.h     — io_uring submit/completion wrapper (backend, ARCH_WITH_URING)
  arch/prefetch.h          — io_uring readahead prefetcher (backend, ARCH_WITH_URING)
  arch/archive.h           — writer/reader high-level API
  util/xattr.h             — POSIX extended attributes
  util/acl.h               — POSIX ACLs (alias header)

src/
  archive/writer.cpp       — writer::impl (add_file, finalize, reader stubs)
  compress/compressor.cpp  — factory registry
  compress/zstd_impl.cpp   — zstd compress/decompress + CRC
  io/pipeline.cpp          — thread-pool stat/open/pread pipeline (planned, Phase 3)
  io/io_ring_pool.cpp      — io_uring SQE/CQE dispatch (ARCH_WITH_URING=ON only)
  io/prefetch.cpp          — windowed async readahead (ARCH_WITH_URING=ON only)
  util/xattr.cpp           — listxattr/getxattr/setxattr

test/
  test_format.cpp          — struct sizes, alignments
  test_compressor.cpp      — zstd roundtrip, factory, corruption
  test_io_uring.cpp        — pool write→read, fsync, multi-write (gated on ARCH_WITH_URING)
  test_prefetch.cpp        — ordered delivery, empty file, is_done (gated on ARCH_WITH_URING)
  test_archive_writer.cpp  — header magic, add_file smoke, options
  test_xattr.cpp           — xattr read roundtrip
  test_crc.cpp             — CRC32C and XXH3
  test_endian.cpp          — htobe*/betoh* roundtrip
  test_dedup_index.cpp     — dedup_entry sort + binary search
  test_footer_serialize.cpp— footer memcpy roundtrip

docs/
  format.md                — archive format specification (v0.2, revised 2026-11)
  api.md                   — library API reference (aligned to v0.2)
  testing.md               — test plan with 50+ future tests
```

## Remaining work (ordered)

> **Phase 0 ✅ and Phase 1 ✅ (2026-11):** the project configures, compiles,
> and tests green (40/40 default, 46/46 `-DARCH_WITH_URING=ON`), and both the
> spec docs and `format.h`/tests are on the frozen v0.2 layout. Next up is
> **Phase 2 (writer serialization + streaming)**; the reader and CLI come
> *after* serialization, since they parse bytes the writer must first produce.
>
> **Design decisions frozen (2026-11):** io_uring demoted to an optional
> backend (`ARCH_WITH_URING`, default OFF) — the primary I/O path is a
> pread worker-thread pipeline **across files**; dedup identity moves to
> **XXH3-128**; TOC sections stay **self-contained** (cross-section dedup
> dropped); `file_entry` gains **symlink-target and hardlink fields**; append
> promises **strict fsync ordering + past-EOF truncation recovery**.

### Phase 0: Get it configuring & compiling — ✅ DONE (2026-11)
All items below landed; default build 40/40 tests green, `-DARCH_WITH_URING=ON`
46/46. Two hangs fixed beyond the listed items (io_uring_queue_init FLAGS
misuse + drain_and_stop deadlock, see status table).
1. **CMake configure fixes** —
   - CMake ≥ 4 rejects crc32c 1.1.2's `cmake_minimum_required(VERSION 3.1)`;
     configure with `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (or bump crc32c).
   - `catch_discover_tests()` is undefined: FetchContent'd Catch2 does not
     propagate its `extras/` modules into the parent scope. Add
     `list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)` and
     `include(Catch)` before `add_subdirectory(test)`.
   - `spdlog::spdlog_headerless` does not exist — use `spdlog::spdlog`
     (or `spdlog::spdlog_header_only`) in all three `target_link_libraries`.
   - Add `option(ARCH_WITH_URING "Build io_uring backend" OFF)`; when OFF,
     exclude `src/io/io_uring_pool.cpp`, `src/io/prefetch.cpp` and their tests
     from the build and skip the liburing `pkg_check_modules`.
   - Create the stub `test/test_toc_serialize.cpp` listed in
     `test/CMakeLists.txt:15`.
2. **Header/include fixes** —
   - `include/arch/format.h:22`: `BLOCK_SIZE` collides with the `sys/mount.h`
     macro — rename to `ARCH_BLOCK_SIZE` everywhere.
   - `include/arch/zstd_impl.h`: inherits from a forward-declared `compressor`
     — include `arch/compressor.h` instead (header is not self-contained).
   - `writer.cpp`: static `add_file_impl(writer::impl*, …)` names the private
     pimpl — make it a member or `friend`.
   - Only with `ARCH_WITH_URING=ON`: `include/arch/io_uring_pool.h:4`:
     `#include <uring.h>` → `<liburing.h>`; add `<algorithm>` to
     `io_uring_pool.cpp`; `next_tag()` touches private `m_next_tag` — make it
     a member or `friend`.
3. **Fix third-party API misuse** — google/crc32c exposes
   `crc32c::Extend(crc, buf, size)` / `crc32c::Value(buf, size)`; there is no
   `crc32c::Crc32c` class or `crc32c::StringView` (`writer.cpp` x2,
   `zstd_impl.cpp`). In zstd: the param is `ZSTD_c_nbWorkers`, not
   `ZSTD_c_nbThreads` — set it to **1** (parallelism comes from the Phase 3
   block pool); `ZSTD_CStreamWorkspaceBound` does not exist;
   `unique_ptr<void, void(*)(void*)>` needs explicit casts to accept
   `ZSTD_createCCtx`/`ZSTD_freeCCtx`. Only with `ARCH_WITH_URING=ON`: replace
   `io_uring_queue_quiesce` (gone in liburing 2.x).
4. **Fix prefetcher (io_uring backend — only matters with `ARCH_WITH_URING=ON`)** —
   `src/io/prefetch.cpp`:
   - Lines 48-72: `chunk` is a raw pointer (`new chunk()`), so `chunk.get()`
     at line 59 is a **compile error**, not just a leak risk. Create the
     `unique_ptr` first and capture `chunk.get()` in the lambda.
   - Lines 100-101: `delete *it;` followed by `m_chunks.erase(it)` is a
     **double-free** — erasing the `unique_ptr` already deletes the chunk.
     Drop the explicit `delete`.
5. **Fix dedup block entries** — `writer.cpp:163-175`: on a dedup hit the
   pushed `block_entry` leaves `compressed_size`, `crc32c`, and
   `compressor_id` zeroed — copy them from the original block's entry.
   (The 128-bit `content_id` itself arrives in Phase 1.)
6. **Get the baseline tests green** — link `Catch2::Catch2WithMain` (no test
   file defines `main`); fix `zstd_mt` → `zstd_impl` at
   `test_compressor.cpp:55`; gate `test_io_uring.cpp` / `test_prefetch.cpp`
   on `ARCH_WITH_URING`.

### Phase 1: Format design freeze (`docs/format.md` + `format.h` + `docs/api.md`)
The first archive ever written locks the format in — settle every open
question in the spec **before** serialization code exists:

> **Status 2026-11: DONE** — `docs/format.md` revised to v0.2 with canonical
> offset tables, `docs/api.md` aligned, and the decisions ported into
> `include/arch/format.h` (48 B `block_entry`/`digest128 content_id`, 128 B
> `file_entry` with symlink/hardlink ids, 128 B `toc_header` with codec+level+
> metadata_crc, `*_SIZE` constants, `__BYTE_ORDER__`-dispatched endian helpers)
> plus updated size tests. Writer switched to XXH3-128 hashing and
> self-describing dedup hits (Phase 0 item 5 completed properly on the way).
> **Remaining for full item-12 effect:** writer must *record* the pinned
> level/id into the serialized `toc_header` — Phase 2.

7. **Pin the block offset convention** — self-contained sections:
   `block_entry.offset` is relative to the section's TOC header (as
   `docs/format.md` already documents). The writer's data-region-relative
   offsets and the absolute-16 write in `finalize()` go away in Phase 2.
8. **TOC CRC covers metadata only** — `toc_header.crc32c` and
   `toc_section_ref.crc32c` cover header + entries + string table, NOT data
   blocks, so `arch list` never reads data (data integrity is already covered
   per-block). Today's "CRC32C of entire TOC section" would force reading the
   whole archive just to validate metadata.
9. **XXH3-128 `content_id`** — `block_entry.content_id` grows to 16 bytes
   (block_entry 32 B → 48 B); `dedup_entry.content_hash` likewise, index
   stays sorted by hash. Removes the silent-collision corruption risk of
   64-bit dedup identity. Update `test_format.cpp` / `test_dedup_index.cpp`.
10. **Canonical per-field byte layouts** — give `file_entry`, `block_entry`,
    `toc_header`, `dedup_entry` and the TOC table explicit offset-tables in
    `docs/format.md`, like the footer already has. C structs become
    in-memory helpers serialized field-by-field — never `memcpy`'d. (The
    api.md line "148 bytes, variable depending on compiler padding" must
    disappear.)
11. **`file_entry` additions** — `symlink_target_id` (string-table reference;
    fixes the discarded-target bug) and `link_target_id` (string-table
    reference to the first-archived path sharing this inode, 0 otherwise →
    extraction re-creates real hardlinks via `link()`; the writer keeps an
    inode→path map during `add_tree`).
12. **Codec + level pinned per section** — TOC header records compressor ID
    and compression level, upholding "same input + same codec + same level ⇒
    identical bytes" (which also lets a dedup hit be verified against the
    original's stored `crc32c`).
13. **Paths & durability contract** — entries stored *relative* to the
    `add_tree()` root; extraction rejects absolute and `..`-escaping names
    (zip-slip). Append write order: data → fsync → TOC → fsync → TOC table +
    footer → fsync; readers treat references extending past EOF as a clean
    truncation point. Update `docs/api.md` (`add_tree`, revised
    `threads`/`prefetch_window` semantics, pipeline options).

### Phase 2: Writer serialization + streaming
14. **Stream blocks to disk** — replace the in-RAM `data_region` vector:
    `add_file`/`add_tree` `pwrite` each compressed block as it is produced,
    at `section_start + data_offset`. Buffer only TOC metadata + dedup index
    (bounded memory, independent of archive size).
15. **TOC serialization** — write `file_entry[]`, `block_entry[]`, string
    table (+ symlink targets, xattr/ACL blobs) and the dedup index
    field-by-field in big-endian via the `htobe*_safe` helpers; patch
    `toc_header` with sizes and the metadata CRC32C. Fix the string-table
    `pwrite` at offset 0 (`writer.cpp:284`).
16. **Footer serialization** — TOC table + footer after the section; compute
    signature CRC; `pwrite` at filesize − 256; fsync per item 13.
17. **`writer::add_tree(path)`** — recursive walk as a first-class operation:
    directory entries, symlinks (store target), hardlink detection
    (inode→path map), relative path names. Serial `os.walk`-style loops
    re-introduce the per-file round-trip cost the pipeline exists to hide.

### Phase 3: I/O pipeline (the network-FS fast path)
18. **File-level pipeline** — new `arch::io::pipeline`: a worker-thread pool
    runs stat → open → pread on **N files in flight simultaneously** across
    the whole file list (N configurable, default ~32), pushing readable
    blocks onto an ordered queue for the writer. This — not within-one-file
    readahead — is what hides NFS/SMB round-trip latency. Plain `pread`
    threads: works on any kernel and any FS, no io_uring dependency.
19. **Block-compression pool** — `options.threads` workers compress ready
    blocks from *all* in-flight files; the writer drains completed blocks in
    file order (Phase 2 item 14). zstd stays single-threaded per block — the
    pool is the parallelism knob (replaces the old "threads passed to the
    compressor" semantics).
20. **(Optional) io_uring backend** — with `ARCH_WITH_URING=ON`, implement
    the same pipeline interface on `io_uring_pool` (fix its O(N) callback
    vector → hash map first) for a local-NVMe fast path.

### Phase 4: Reader
21. **Deserialization** — `pread` footer, validate self-check + signature
    CRCs, read TOC table, parse section **metadata only** (item 8 — `list()`
    never touches data blocks), build in-memory entry list.
22. **Split out `src/archive/reader.cpp`** — currently stubs inside
    `writer.cpp`. Block decompression, file reconstruction, symlink and
    hardlink re-creation (item 11), xattr/ACL restore hooks.
23. **Extraction pipeline** — mirror of Phase 3 on the restore side:
    concurrent block reads + parallel decompression + async output writes.
    A serial single-threaded `extract()` reproduces the network-FS latency
    problem exactly.
24. **Roundtrip tests** — create → list → extract → byte-compare, incl.
    symlinks, hardlinks, xattrs; fill in `test_toc_serialize.cpp`.

### Phase 5: CLI
25. **Create `src/main.cpp`** — `arch create <archive> <tree|files...>`,
    `arch extract <archive> [dest]`, `arch list <archive>`,
    `arch append <archive> [files...]`, built on `add_tree`/reader. spdlog
    progress. (Referenced by `CMakeLists.txt:86` — generation fails until it
    exists.)

### Phase 6: Append support
26. **`writer::append()`** — open existing archive, `pread` + validate
    footer, write a new self-contained TOC section (with its own data and
    dedup index) at EOF, rewrite TOC table + footer in the item-13 order.
    Dedup does **not** cross sections (frozen decision: self-containment
    wins over cross-append space savings).

### Phase 7: Full xattr/ACL
27. **ACL support** — finish `util::read_acl` (`getfacl`), `util::write_acl`
    (`setfacl`). Wire into writer/reader.
28. **ACL roundtrip tests** — verify ACLs survive archive→extract cycle.

### Phase 8: Robustness
29. **Corruption detection** — reader verifies block CRC32C before
    decompressing, file CRC32C after decompression, TOC metadata CRC32C on
    parse. Report which block/file failed.
30. **Sparse file support** — detect sparse regions via `SEEK_DATA`/`SEEK_HOLE`,
    archive holes without reading them (a big win on network FSes too).
31. **Error recovery** — pipeline worker error propagation, retry with
    backoff for transient network-FS errors (EIO/EAGAIN/ESTALE on NFS).

### Phase 9: Performance
32. **Benchmark suite** — throughput on local SSD, NFS (simulated latency),
    SMB; compare against `tar --zstd` and `bsdtar -z`. Include a many-small-
    files workload — that is the pipeline's reason to exist.
33. **Memory profiling** — per-thread zstd context reuse, pipeline buffer
    pool sizing, dedup index RAM cost (≈ 16 GB per PiB unique data at 4 MiB
    blocks — document and consider an on-disk spill index).

> Removed from roadmap: **dedup cross-section** (frozen decision — TOC
> sections stay self-contained) and **per-file zstd multithreading** as a
> parallelism knob (the block pool owns parallelism; `nbWorkers=1`).

## Dependencies

| Dep | Origin | Version | Notes |
|--|--|--|--|
| spdlog | FetchContent (GitHub) | v1.15.3 | Logging. Transitively brings in fmt. |
| zstd | FetchContent (GitHub) | v1.5.7 | Static only, programs disabled. |
| crc32c | FetchContent (GitHub) | 1.1.2 | Hardware-accelerated CRC32C. |
| xxHash | FetchContent (GitHub) | v0.8.3 | XXH3 for dedup hashing (XXH3-128 after Phase 1 freeze). |
| liburing | pkg-config (system) | ≥ 2.9 | io_uring userspace lib — **optional**, only needed with `ARCH_WITH_URING=ON`. |
| Catch2 | FetchContent (GitHub) | v3.9.1 | Test framework. |

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

io_uring backend is opt-in: add `-DARCH_WITH_URING=ON` (requires liburing
headers). Default builds are plain POSIX — no liburing needed.

**Requires:** CMake ≥ 3.22, C++20 compiler (the crc32c policy workaround is
baked into CMakeLists via `CMAKE_POLICY_VERSION_MINIMUM`). liburing headers
only with `-DARCH_WITH_URING=ON`.
**Status: builds & tests green** — 40/40 default, 46/46 with the uring flag
(Phase 0 complete).

## Known issues / tech debt

*Resolved 2026-11 in Phases 0–1 (was the bulk of this list): everything
compile-related, the double-free + compile error in `prefetch.cpp`, the
dedup-hit field gap, the string-table-pwrite-clobbers-header bug, the
64-bit-dedup/TOC-CRC/offset-convention decisions (now in `format.h` + v0.2
docs), unconditional bswaps (now `__BYTE_ORDER__`-dispatched), and all test
breakage. Plus two live bugs found while getting the uring backend green:
`io_uring_queue_init`'s 3rd arg is FLAGS (ring was left `R_DISABLED`) and
`drain_and_stop()` deadlocked workers parked in `io_uring_wait_cqe`.*

Open items:

- **`writer.cpp:69`** — magic write uses comma expressions; cosmetic only —
  all four bytes *are* assigned and match `ARCH_MAGIC`. Prefer `std::memcpy`
  when Phase 2 touches this code.
- **Symlink target stored but unreferenced** — `add_file_impl` appends the
  target to the string table but doesn't yet set `file_entry::symlink_target_id`
  (field exists since Phase 1); lands with Phase 2 serialization.
- **`block_entry.offset` is still data-region-relative in the writer** —
  **frozen convention is section-relative (item 7)**; changes in Phase 2.
- **`finalize()` buffers everything in RAM** and TOC/footer serialization is
  stubbed — Phase 2 item 14 (streamed `pwrite`s, bounded memory).
- **Hardlink inode→path map** — `link_target_id` exists in the struct but no
  writer populates it yet (needs `add_tree`, Phase 2 item 17).
