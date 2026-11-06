# AGENTS.md — Archiver

A C++20 archiver library optimized for network filesystems. Custom on-disk format (not libarchive), with parallel per-file compression and io_uring-based prefetch.

## Build & Test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

- CMake 3.22+, C++20 compiler, `liburing-devel`/`liburing-dev` (system lib via pkg-config).
- All other deps (spdlog, zstd, crc32c, xxHash) are fetched via FetchContent — no manual install needed.
- Tests: single binary `arch_test` driven by Catch2 v3 + `catch_discover_tests()`. Tags like `[slow]` mark heavy tests.

## Code Layout

```
include/arch/        — public API headers (format, compressor, io_uring_pool, prefetch, archive)
include/util/        — xattr + ACL helpers
src/archive/         — writer/reader high-level implementation (both in writer.cpp for now)
src/compress/        — compressor factory + zstd impl
src/io/              — io_uring pool + prefetcher
src/util/            — xattr implementation
test/                — Catch2 tests (one file per subsystem)
docs/                — format spec, test plan, API reference
```

`src/main.cpp` (the CLI tool referenced in CMakeLists.txt) does not exist yet — first thing to create when adding a CLI.

## Architecture & Data Flow

- **writer / reader** use pimpl (`writer::impl`, `reader::impl`) defined inside `writer.cpp`.
- **Archive format**: seek-based (no streaming). Layout is `archive_header (16B) → data blocks → file entries → string table → TOC section → footer (256B at EOF)`. See `docs/format.md` for exact struct layouts.
- **Compressor plugin system**: factory registry (`register_compressor` / `make_compressor`) with `REGISTER_COMPRESSOR` macro for static init. Each compressor works on single 4 MiB blocks.
- **io_uring pool**: wraps raw io_uring ring, dispatches callbacks via a `std::vector` of tag→callback pairs. Callbacks fire on whichever worker thread picks up the CQE.
- **Prefetcher**: reads ahead via io_uring; caller pulls chunks in offset order via `pop_ready_chunk()` (blocks until next chunk is ready).

### Namespace Conventions
- `arch::format` — on-disk structs and magic constants
- `arch::compress` — compressor base class + factory + zstd impl
- `arch::io` — io_uring pool and prefetcher
- `arch` — writer and reader high-level classes
- `util` — xattr, ACL helpers

## Gotchas & Non-Obvious Patterns

### `writer.cpp` bugs (documented in PLAN.md)
- **Line 69**: magic header uses comma expressions (`hdr[0] = 'A', hdr[1] = 'R'`) — this only assigns `hdr[0]`. Should use `std::memcpy` or initializer list.
- **Line 129**: `std::filesystem::read_symlink` — not standard C++20 yet. Use POSIX `readlink()` instead.
- **Line 284**: `pwrite` at offset 0 for string table overwrites archive header — needs proper offset calculation.
- **`finalize()`** is incomplete — TOC section and footer serialization are stubs.

### `prefetch.cpp` ownership bug
- **Lines 48-72**: `chunk` is created with `new`, then raw pointer captured via `.get()` before `unique_ptr` takes ownership at line 72. Exception between lines 48-72 leaks. Fix: create `unique_ptr` first, capture `.get()` in the lambda.

### `io_uring_pool.cpp` performance issue
- **Lines 153-160**: callback lookup is O(N) linear search in a `std::vector`. Should be `std::unordered_map<uint64_t, io_callback>` for high-throughput.

### Endianness
- On-disk structs use the `htobe*/betoh*` helpers in `format.h` (which use `__builtin_bswap*`). Always convert before/after writing to disk — structs use native endianness in memory.

### Dedup lookup mismatch
- In `writer.cpp` dedup path (line 163-175): when a dedup hit is found, `compressed_size` and `crc32c` are NOT copied from the original `block_entry` — the dedup block_entry is incomplete. The dedup path looks up the physical offset but doesn't copy the full block entry.

### Compressor factory thread safety
- `g_registry` is protected by `g_registry_mtx`, but `make_compressor` locks, finds the factory, copies the `std::function`, then calls it. The factory `std::function` may outlive the lock guard — make sure registered factories have static lifetime (they do, via the `REGISTER_COMPRESSOR` macro pattern).

### Test CMakeLists references missing file
- `test/CMakeLists.txt` lists `test_toc_serialize.cpp` which does not yet exist on disk. The `test_footer_serialize.cpp` exists but tests footer, not TOC.

### Test bug
- `test_compressor.cpp:55` constructs `zstd_mt comp(4)` — no such class exists. Should be `zstd_impl comp(4)`.

### Header include style
- Headers use `#include "arch/format.h"` (bracket quotes, relative to `include/`). Source files use the same bracket-style convention.

### File descriptor lifecycle
- `writer` and `reader` manage raw `int` file descriptors (not RAII wrappers). On destruction, they close if `fd >= 0`. The writer's destructor does NOT auto-finalize.

### Small vs large file heuristic
- Files < 16 MiB (`BLOCK_SIZE * 4`) are treated as "small" (prefetcher path). The `use_prefetch` flag is set but not yet meaningfully wired up — the prefetch code path in `add_file_impl` is still a no-op.

## Test Conventions
- Catch2 v3. `TEST_CASE("name", "[tag1][tag2]")`.
- `[slow]` tag for anything >5s, >64 MiB alloc, or requiring network FS.
- Use `Catch::Matchers::Equals` for vector comparison.
- Tests use `mkstemp` for temp files (not Catch2's file helpers).

## Phase Checklist (current status)
See PLAN.md for full status. In short: format structs ✅, compression ✅, io_uring pool ✅, prefetcher ⚠️ (bug), writer ⚠️ (incomplete finalize), reader ❌ (stubs), CLI ❌ (missing). Work in Phase 1 order before Phase 2+.
