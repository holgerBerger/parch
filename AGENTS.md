# AGENTS.md — Archiver

A C++20 archiver library optimized for network filesystems. Custom on-disk format (not libarchive), with a pread worker-thread pipeline across files, pluggable block compression, and an optional io_uring backend (`ARCH_WITH_URING`, default OFF).

## Build & Test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

- **Builds green (2026-11, Phase 0 done):** 40/40 tests default; 46/46 with `-DARCH_WITH_URING=ON`. The policy flag is set inside CMakeLists (`CMAKE_POLICY_VERSION_MINIMUM`), not required on the command line anymore.
- CMake 3.22+, C++20 compiler. Note: GCC 15 rejects `opts = {}` default arguments for nested classes with only implicit ctors — `writer::options` has an explicit `options() {}` for this; don't remove it. Default build is plain POSIX — **liburing is NOT required**.
- io_uring backend is opt-in: `-DARCH_WITH_URING=ON` (needs `liburing-devel`/`liburing-dev` via pkg-config) and pulls in `src/io/io_uring_pool.cpp`, `src/io/prefetch.cpp` + their tests.
- All other deps (spdlog, zstd, crc32c, xxHash) are fetched via FetchContent — no manual install needed.
- Tests: single binary `arch_test` driven by Catch2 v3 + `catch_discover_tests()`. Tags like `[slow]` mark heavy tests.

## Code Layout

```
include/arch/        — public API headers (format, compressor, archive, io pipeline;
                       io_uring_pool + prefetch only with ARCH_WITH_URING)
include/util/        — xattr + ACL helpers
src/archive/         — writer/reader high-level implementation (both in writer.cpp for now)
src/compress/        — compressor factory + zstd impl
src/io/              — file-I/O pipeline (planned); io_uring pool + prefetcher (ARCH_WITH_URING only)
src/util/            — xattr implementation
test/                — Catch2 tests (one file per subsystem; io_uring tests gated on ARCH_WITH_URING)
docs/                — format spec, test plan, API reference
```

`src/main.cpp` is a stub (usage banner) so the `arch` target builds; real commands land in PLAN.md Phase 5.

## Architecture & Data Flow

- **writer / reader** use pimpl (`writer::impl`, `reader::impl`) defined inside `writer.cpp`.
- **Archive format**: seek-based (no streaming format), but writes are **streamed** — blocks `pwrite`d as produced, only TOC metadata + dedup index buffered in RAM. Layout is `archive_header (16B) → [self-contained TOC sections: data blocks + file entries + block entries + string table (+ dedup index)] → TOC table → footer (256B at EOF)`. Spec: `docs/format.md` **v0.2** (2026-11 freeze) — `format.h` structs and size tests are ported to v0.2 (serialization still pending, Phase 2).
- **I/O pipeline (planned, Phase 3)**: worker-thread pool stats/opens/preads **N files concurrently** (the network-FS latency fix — across files, not within one), feeding a block-compression pool sized by `options.threads`. zstd runs `nbWorkers=1`; the pool is the parallelism knob.
- **io_uring backend (optional, `ARCH_WITH_URING=ON`)**: `io_uring_pool` wraps a raw io_uring ring, dispatches callbacks via a `std::vector` of tag→callback pairs; the `prefetcher` does within-file readahead on it. Slated to become one backend of the pipeline interface — not the primary path.
- **Compressor plugin system**: factory registry (`register_compressor` / `make_compressor`) with `REGISTER_COMPRESSOR` macro for static init. Each compressor works on single 4 MiB blocks; codec **and level are pinned per TOC section** (Phase 1).

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

### `writer.cpp` — remaining known gaps (Phase 2 in PLAN.md)
- **Magic write uses comma expressions** (`hdr[0] = 'A', hdr[1] = 'R', …`) — correct but cosmetic; prefer `std::memcpy`.
- **Symlink target** is appended to the string table but its offset isn't yet stored in a serialized field — `file_entry::symlink_target_id` exists in `format.h` (v0.2); wiring it happens with Phase 2 serialization.
- **`finalize()`** still buffers the data region in RAM and the TOC section/footer serialization are stubs (offsets no longer clobber the header — string table moved to EOF-chaining — but Phase 2 replaces the whole scheme with section-relative streaming writes).
- **`block_entry.offset`** is currently data-region-relative in the writer; **frozen convention is section-relative** (PLAN Phase 1 item 7) — changes with Phase 2.

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
See PLAN.md for full status. In short: **Phase 0 ✅, Phase 1 ✅** (docs + `format.h` + tests all on v0.2; builds green both flag settings), pipeline ❌ (not written — Phase 3, the network-FS fast path), writer ⚠️ (compiles; still buffers everything in RAM, TOC/footer serialization stubbed — Phase 2), reader ❌ (stubs — Phase 4), CLI ⚠️ (stub `main.cpp` exists — real commands in Phase 5). Next up: **Phase 2 (writer serialization + streaming)** → Phase 3 (pipeline) → Phase 4 (reader) → Phase 5 (CLI).
