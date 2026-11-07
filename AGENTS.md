# AGENTS.md — Archiver

A C++20 archiver library optimized for network filesystems. Custom on-disk format (not libarchive), with a pread worker-thread pipeline across files, pluggable block compression, and an optional io_uring backend (`ARCH_WITH_URING`, default OFF).

## Build & Test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

- **Builds green (2026-11, Phases 0–4 done):** 66/66 tests default; 72/72 with `-DARCH_WITH_URING=ON`. The policy flag is set inside CMakeLists (`CMAKE_POLICY_VERSION_MINIMUM`), not required on the command line anymore.
- CMake 3.22+, C++20 compiler. Note: GCC 15 rejects `opts = {}` default arguments for nested classes with only implicit ctors — `writer::options` has an explicit `options() {}` for this; don't remove it. Default build is plain POSIX — **liburing is NOT required**.
- io_uring backend is opt-in: `-DARCH_WITH_URING=ON` (needs `liburing-devel`/`liburing-dev` via pkg-config) and pulls in `src/io/io_uring_pool.cpp`, `src/io/prefetch.cpp` + their tests.
- All other deps (spdlog, zstd, crc32c, xxHash) are fetched via FetchContent — no manual install needed.
- Tests: single binary `arch_test` driven by Catch2 v3 + `catch_discover_tests()`. Tags like `[slow]` mark heavy tests.

## Code Layout

```
include/arch/        — public API headers (format, compressor, archive, io pipeline;
                       io_uring_pool + prefetch only with ARCH_WITH_URING)
include/util/        — xattr + ACL helpers
src/archive/         — writer.cpp (writer + static append), reader.cpp (reader/extract),
                       format_parse.h (internal field-by-field BE parsers shared by both)
src/compress/        — compressor factory + zstd impl
src/io/              — pipeline.cpp (Phase 3, always built); io_uring pool + prefetcher (ARCH_WITH_URING only)
src/util/            — xattr implementation
test/                — Catch2 tests (one file per subsystem; io_uring tests gated on ARCH_WITH_URING)
docs/                — format spec, test plan, API reference
```

`src/main.cpp` is a stub (usage banner) so the `arch` target builds; real commands land in PLAN.md Phase 5.

## Architecture & Data Flow

- **writer / reader** use pimpl (`writer::impl` in `writer.cpp`, `reader::impl` in `reader.cpp`).
- **Archive format**: seek-based (no streaming format), but writes are **streamed** — blocks `pwrite`d as produced, only TOC metadata + dedup index buffered in RAM. Layout is `archive_header (16B) → [self-contained TOC sections: data blocks + file entries + block entries + string table (+ dedup index)] → TOC table → footer (256B at EOF)`. Spec: `docs/format.md` **v0.2** (2026-11 freeze) — `format.h`, the writer serializers, and the size/conformance tests are all on v0.2.
- **I/O pipeline (Phase 3, `arch::io::pipeline`, always built)**: `files_in_flight` worker threads open+fstat+pread **N files concurrently** (the network-FS latency fix — across files, not within one), each raw block is XXH3-hashed (dedup) and handed to `options.threads` compression workers writing into per-file ordered slots; the writer consumes in submit order (single consumer ⇒ archive bytes are scheduling-independent). Bounded memory: `block_permits` live blocks + per-file liveness cap. `writer::add_tree` routes regular files through it; `options.threads = 0` selects the serial fallback. zstd runs `nbWorkers=1`; the pool is the parallelism knob.
- **io_uring backend (optional, `ARCH_WITH_URING=ON`)**: `io_uring_pool` wraps a raw io_uring ring, dispatches callbacks via an `unordered_map` of tag→callback (CQE reaping serialized under `m_cq_mtx`); the `prefetcher` does within-file readahead on it. Slated to become one backend of the pipeline interface — not the primary path.
- **Compressor plugin system**: factory registry (`register_compressor` / `make_compressor`) with `REGISTER_COMPRESSOR` macro for static init. Each compressor works on single 4 MiB blocks; codec **and level are pinned per TOC section** (Phase 1). Readers resolve codecs via `make_compressor_by_id(id, level)` (registry probe + id→name cache).
- **Reader (Phase 4, `src/archive/reader.cpp`)**: constructor runs the full metadata validation chain but reads **no data blocks** (TOC CRCs cover metadata only). `extract_all` runs four waves — dirs serial (sorted) → files/symlinks/FIFOs/devices on a `reader::options.threads` pool (default = hardware concurrency; per-thread decompressor cache, first-error capture) → hardlinks via `link()` (falls back to extracting the owner's blocks) → directory metadata reverse-order (children must be in place first). Blocks verified by CRC32C (compressed) then file checksum (uncompressed chain); extraction names go through the zip-slip guard.
- **`writer::append()` is a static factory** `static writer append(path, opts = {})` (the old member was impossible — the normal ctor `O_TRUNC`s the file): validates the existing archive, keeps the valid section prefix, `ftruncate`s away the old TOC table + footer, inherits refs/totals and the DEDUP footer flag; the returned writer then streams one more section via the normal add*/finalize path.

### Frozen design decisions (v1 — do not re-litigate without updating PLAN.md)
- **Self-contained TOC sections** — all offsets section-relative; dedup never crosses sections (cross-section dedup removed from roadmap).
- **XXH3-128 dedup identity** — `content_id` is 16 bytes (block_entry 48 B); 64-bit was rejected as silently corrupting on collision.
- **TOC CRC covers metadata only** (header + entries + strings) so `list()` never reads data blocks.
- **Durable append**: data → fsync → TOC → fsync → TOC table + footer → fsync; references past EOF = clean truncation point.
- **Relative paths only** in the string table (rooted at the `add_tree()` argument); extraction rejects absolute / `..`-escaping names.
- **Symlink targets and hardlinks are first-class** — `file_entry` gains `symlink_target_id` and `link_target_id` (Phase 1 item 11).
- **io_uring is optional**; the default I/O path is portable POSIX threads + `pread`.

### Namespace Conventions
- `arch::format` — on-disk structs and magic constants
- `arch::compress` — compressor base class + factory + zstd impl
- `arch::io` — I/O pipeline; with `ARCH_WITH_URING`: pool + prefetcher backends
- `arch` — writer and reader high-level classes
- `util` — xattr, ACL helpers

## Gotchas & Non-Obvious Patterns

### `writer.cpp` — serialization notes (Phase 2 done)
- **Section layout**: `ensure_section()` reserves a zeroed 128-byte `toc_header` at `file_end`; blocks are `pwrite`d live at `section_start + 128 + data_written`; `finalize_section()` appends the serialized tables and patches the header in place. `block_entry.offset` is **section-relative** (frozen convention); `data_offset` is always `TOC_HEADER_SIZE`.
- **Serializers are field-by-field** per `docs/format.md` offset tables (`serialize_file_entry` etc. in an anonymous namespace with size `abort()` guards) — never `memcpy` a struct. `test/test_writer_serialize.cpp` re-parses archives independently.
- **String table byte 0 is a reserved NUL sentinel** (`add_string` starts at offset ≥ 1; 0 = "absent" in all ref fields). Xattr/ACL blobs also live in the string table (`add_blob`, no NUL — the xattr format self-terminates); lengths are big-endian per spec (`src/util/xattr.cpp` now matches).
- **Append factory** (`writer::append`): uses `format_parse.h`; on section validation failure it treats that section as a clean truncation point (drop it + later), recomputes kept totals from the entry tables, and only trusts the old footer's totals when nothing was dropped.

### `pipeline.cpp` — Phase 3 notes
- **Consumption contract** (`include/arch/pipeline.h`): `submit()` → repeated `next_block(seq, i, blk)` + `release_block(seq)` → `wait_file_done(seq)` exactly once. `next_block` only returns false at a real gap: it waits for the slot, the file error, or `io_done && queued_raw == 0 && in_compress == 0` (don't simplify this — waking on `io_done` alone was a race that lost the last block when compression lagged behind I/O).
- **Permit accounting is exactly-once**: io_worker acquires before each pread; release happens in `release_block` (writer), in the compress worker's drop paths (error / slot vanished / file erased — it re-looks-up `seq` under the store lock), or in `wait_file_done` (stranded slots only: `permits_held - queued_raw - in_compress`). Any change must keep those three disjoint.
- **Liveness bound**: `files_in_flight * per_file_cap < block_permits` (enforced by the ctor by scaling permits up). Without the per-file cap the head file's io thread can starve behind the other in-flight files and the writer deadlocks in `next_block`.
- The pipeline takes its **own fstat** (submit's size arg is only a slot-count estimate) and reads xattrs/ACLs in the io worker — those round-trips are part of what cross-file concurrency hides. Writer metadata comes from the terminal `pipeline_file`.
- `add_tree` error path reaps in-flight files (`reap_rest`) so the writer stays usable after a throw; `wait_file_done` on an already-erased seq is a safe no-op.
- **Linking note**: `REGISTER_COMPRESSOR` uses static initializers — consumers linking `libarchiver_lib.a` statically must use `-Wl,--whole-archive` (or reference a `zstd_impl` symbol) or `make_compressor("zstd")` fails at runtime. Phase 5 CLI should link accordingly or add explicit registration.

### `reader.cpp` — Phase 4 notes
- **Parsers live in `src/archive/format_parse.h`** (field-by-field BE, mirroring `docs/format.md`; also used by `writer::append`). Tests keep *independent* re-parsers — a symmetric writer/reader bug must not hide.
- **Metadata CRC recompute needs no re-serialization**: read the raw section bytes, copy the 128-byte header with bytes [92,96) zeroed, CRC over that + the contiguous tail from `file_table_offset` to section end. Compare against BOTH `toc_header.metadata_crc` and the `toc_section_ref.metadata_crc`.
- **Section bounds checks come before any tail read** (`parse_sections`) — a malicious/rotten header must never cause an out-of-range `pread` length.
- **`by_path` is last-occurrence-wins** (append = versioning); `list()` still reports every entry from every kept section.
- **`block_entry.offset` is section-relative from the `toc_header`** and the data region is `[data_offset, data_offset+data_size)`; extract validates blocks against it.
- **Hardlink owner resolution** goes through `by_path` (the owner may live in an earlier section); if the owner path doesn't exist on disk at wave-3 time (overwritten by a later regular file, or section dropped), wave 3 extracts the owner's blocks under the duplicate's name instead of linking.
- **Decompressors are per-thread** (`codec_cache` per pool worker) — zstd CCtx is not thread-safe. The reader shares one `arc_fd` across threads via `pread` only.
- **`extract_all` wave order is load-bearing**: parents before children (workers don't mkdir-race), files before hardlinks (link target must exist), children before directory metadata (a ro-mode dir would block writing into it, and times must be set after contents).
- xattr restore: `util::write_xattrs(dest, strings.substr(xattr_id))` — the blob is self-terminating (4-byte zero length), so slicing to the end of the string table is safe.

### Resolved (was: Phase 0 compile blockers)
- `format.h`: renamed `BLOCK_SIZE` → `ARCH_BLOCK_SIZE` (collides with `sys/mount.h` macro); ported to v0.2 layouts (48 B `block_entry` w/ 16-byte `digest128 content_id`, 128 B `file_entry` w/ `symlink_target_id`/`link_target_id`, 128 B `toc_header`, canonical `*_SIZE` constants). Structs are in-memory helpers — never `memcpy`'d; `file_entry` declaration order groups u64→u32→u16 + tail pad to hit exactly 128 B (serialization order follows docs/format.md tables instead).
- Third-party APIs fixed: crc32c exposes `Extend(uint32_t, const uint8_t*, size)` / `Crc32c(...)` (**no `Value`, no `StringView`, no `Crc32c` class**); zstd param is `ZSTD_c_nbWorkers` (pinned 1), constructor takes the **compression level**; `ZSTD_CStreamWorkspaceBound` doesn't exist (`mem_usage_hint` uses `ZSTD_compressBound`).
- `add_file_impl` is a member of `writer::impl` (was a static naming the private pimpl).
- Dedup hits are self-describing: `dedup_node` keeps the owner's block index; `compressed_size`/`crc32c`/`compressor_id` are copied on a hit; hashing is XXH3-128 (`XXH3_128bits`).
- io_uring backend (`ARCH_WITH_URING=ON`): compiles, tests pass, **verified under ASan/UBSan** (12× clean). Non-obvious fixes to remember: **`io_uring_queue_init`'s third argument is FLAGS, not a CQ depth** (passing 256 set `IORING_SETUP_R_DISABLED` and nothing ever completed) — use `io_uring_queue_init_params`; **workers must use `io_uring_wait_cqe_timeout`**, otherwise `drain_and_stop()` deadlocks on an idle ring; **liburing CQE consumption is not thread-safe** — workers reap under a dedicated `m_cq_mtx` (double-consumed CQEs ran callbacks twice ⇒ use-after-free) and dispatch outside it; `m_pending` (incremented under `m_mtx` before `io_uring_submit`) tracks ops until *callback dispatch completes*, and `drain_and_stop()` waits on a condvar for it. The prefetcher keeps chunks alive via `shared_ptr` held by the io callback itself — never hand the callback a raw pointer the consumer may free.
- Tests: `Catch2::Catch2WithMain` linked; `zstd_mt` typo fixed; `test_toc_serialize.cpp` exists (placeholder + v0.2 header checks); io_uring/prefetch tests are only compiled with `ARCH_WITH_URING=ON`; async-write test buffers must outlive `drain_and_stop()` (kept in a vector, not a loop temporary).

### Endianness
- On-disk structs use the `htobe*_safe`/`betoh*_safe` helpers in `format.h`, now dispatched on `__BYTE_ORDER__` (bswap on LE, identity on BE). Always convert before/after writing to disk — structs use native endianness in memory.

### Header include style
- Headers use `#include "arch/format.h"` (quote-style, relative to `include/`). Source files use the same quote-style convention.

### File descriptor lifecycle
- `writer` and `reader` manage raw `int` file descriptors (not RAII wrappers). On destruction, they close if `fd >= 0`. The writer's destructor does NOT auto-finalize.

### Small vs large file heuristic (superseded)
- The old design routed files < 16 MiB (`BLOCK_SIZE * 4`) through an io_uring within-file prefetcher. **Replaced** by the Phase 3 file-level pipeline (concurrency across files); `use_prefetch` and the threshold will go away with it. Until then the prefetch path in `add_file_impl` is a no-op.

## Test Conventions
- Catch2 v3. `TEST_CASE("name", "[tag1][tag2]")`.
- `[slow]` tag for anything >5s, >64 MiB alloc, or requiring network FS.
- Use `Catch::Matchers::Equals` for vector comparison.
- Tests use `mkstemp` for temp files (not Catch2's file helpers).

## Phase Checklist (current status)
See PLAN.md for full status. In short: **Phases 0–4 ✅** (builds green; `format.h` on v0.2; writer streams + serializes spec-conformant archives with pipelined `add_tree`; reader parses/validates/extracts in a parallel wave pipeline with corruption + truncation-tolerance semantics; `writer::append()` static factory works and is covered by two-section roundtrip tests — 66/66 default, 72/72 `ARCH_WITH_URING=ON`; reader is ASan/UBSan-clean and TSan-clean). CLI ⚠️ (stub `main.cpp` — Phase 5). Next up: **Phase 5 (CLI)**.
