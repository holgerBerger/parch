# AGENTS.md — Archiver

A C++20 archiver library optimized for network filesystems. Custom on-disk format (not libarchive), with a pread worker-thread pipeline across files, pluggable block compression, and an optional io_uring backend (`ARCH_WITH_URING`, default OFF).

## Build & Test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

- **Builds green (2026-11, Phases 0–5, 7–10 done):** 97/97 tests default; 103/103 with `-DARCH_WITH_URING=ON`. The policy flag is set inside CMakeLists (`CMAKE_POLICY_VERSION_MINIMUM`), not required on the command line anymore.
- CMake 3.22+, C++20 compiler. Note: GCC 15 rejects `opts = {}` default arguments for nested classes with only implicit ctors — `writer::options` has an explicit `options() {}` for this; don't remove it. Default build is plain POSIX — **liburing is NOT required**.
- io_uring backend is opt-in: `-DARCH_WITH_URING=ON` (needs `liburing-devel`/`liburing-dev` via pkg-config) and pulls in `src/io/io_uring_pool.cpp`, `src/io/prefetch.cpp` + their tests.
- **libacl is a required system dep** (`pkg_check_modules(LIBACL REQUIRED ...)`) for the ACL capture/restore in `src/util/xattr.cpp` (Phase 7). All other deps (spdlog, zstd, crc32c, xxHash) are fetched via FetchContent — no manual install needed.
- Tests: single binary `arch_test` driven by Catch2 v3 + `catch_discover_tests()`. Tags like `[slow]` mark heavy tests.
- Benchmarks (Phase 9): `cmake --build build --target arch_bench netfs_delay`, then `bench/run_bench.sh` (matrix; env `DATA=<dir>`, results are `key=value` lines — see `docs/benchmarks.md`). `arch_bench` needs the same `--whole-archive` treatment as `parch`.

## Code Layout

```
include/arch/        — public API headers (format, compressor, archive, io pipeline;
                       io_uring_pool + prefetch only with ARCH_WITH_URING)
include/util/        — xattr + ACL helpers; profile.h (shared stage_timer/span
                       mechanism for writer + reader profiling)
src/archive/         — writer.cpp (writer + static append), reader.cpp (reader/extract + verify_all),
                       format_parse.h (internal field-by-field BE parsers shared by both)
src/compress/        — compressor factory + zstd impl
src/io/              — pipeline.cpp (Phase 3, always built), sparse.h (Phase 8 hole helpers);
                       io_uring pool + prefetcher (ARCH_WITH_URING only)
src/util/            — xattr implementation, retry.h (transient-error backoff, header-only)
test/                — Catch2 tests (one file per subsystem; io_uring tests gated on ARCH_WITH_URING)
bench/               — Phase 9: bench.cpp (`arch_bench` in-process timing + RSS harness,
                       key=value result lines), netfs_delay.c (`libnetfs_delay.so`
                       LD_PRELOAD latency shim), run_bench.sh (full matrix)
docs/                — format spec, test plan, API reference, benchmarks.md (Phase 9 results),
                       extract-pipeline.md (Phase 10 design, optional/unimplemented)
```

`src/main.cpp` is the `parch` CLI (Phase 5; "parallel archiver" — the name
expresses the cross-file worker pipeline): `create` / `append` / `list` /
`extract`; options map 1:1 to `writer::options` / `reader::options`
(`-c zstd|none`, `-l`, `-j`, `--depth`, `--dedup`, `--no-sync`, `--no-xattr`,
`--no-acl`, `-C DIR`, `-v`/`-q`). `writer::options.xattrs`/`.acls` (default
true) skip the per-file listxattr/getxattr + acl_get probes when false — a
metadata-RPC saver for MDS-bound network filesystems (real Lustre create was
MDS-rate-bound: the probes were 36% of busy time).
The `parch` target links `archiver_lib` inside a literal
`-Wl,--whole-archive ... -Wl,--no-whole-archive` pair in CMakeLists — required
for the `REGISTER_COMPRESSOR` static init (see linking note below).

## Architecture & Data Flow

- **writer / reader** use pimpl (`writer::impl` in `writer.cpp`, `reader::impl` in `reader.cpp`).
- **Archive format**: seek-based (no streaming format), but writes are **streamed** — blocks `pwrite`d as produced, only TOC metadata + dedup index buffered in RAM. Layout is `archive_header (16B) → [self-contained TOC sections: data blocks + file entries + block entries + string table (+ dedup index)] → TOC table → footer (256B at EOF)`. Spec: `docs/format.md` **v0.3** (0.2 design freeze 2026-11 + Phase 8 sparse blocks) — `format.h`, the writer serializers, and the size/conformance tests are on the v0.2 layouts; v0.3 adds only the `COMPRESSOR_SPARSE` block kind + footer version.
- **I/O pipeline (Phase 3, `arch::io::pipeline`, always built)**: `files_in_flight` worker threads open+fstat+pread **N files concurrently** (the network-FS latency fix — across files, not within one), each raw block is XXH3-hashed (dedup) and handed to `options.threads` compression workers writing into per-file ordered slots; the writer consumes in submit order (single consumer ⇒ archive bytes are scheduling-independent). The walk STREAMS into it — files are submitted as discovered (first bytes land while the tree is still walked; byte-identity vs. the old collect-first walk is test-guarded). Bounded memory: `block_permits` live blocks + per-file liveness cap. `writer::add_tree` routes regular files through it; `options.threads = 0` selects the serial fallback. zstd runs `nbWorkers=1`; the pool is the parallelism knob.
- **io_uring backend (optional, `ARCH_WITH_URING=ON`)**: `io_uring_pool` wraps a raw io_uring ring, dispatches callbacks via an `unordered_map` of tag→callback (CQE reaping serialized under `m_cq_mtx`); the `prefetcher` does within-file readahead on it. Slated to become one backend of the pipeline interface — not the primary path.
- **Sparse files (Phase 8, format v0.3-minimal)**: `writer::options.sparse` (default true) — hole detection is gated on `fstat` (`st_blocks*512 < st_size`) and uses **SEEK_DATA** from each block start (only *whole*-hole blocks are sparse; `src/io/sparse.h::block_is_hole`). Sparse blocks are `block_entry.compressor_id = COMPRESSOR_SPARSE (0x02)` with `compressed_size = 0`, `crc32c` = CRC of zeros, `content_id` = XXH3-128 of zeros (`zero_digest`, cached per length) so dense all-zero blocks dedup-match holes. The footer advertises `0x0003` only when `impl::used_sparse` was set (sticky; `writer::append` inherits it via `footer.version >= FORMAT_VERSION_SPARSE`); append validation accepts versions ≤ `FORMAT_VERSION_SPARSE`. Extraction: `extract_data` skips the pwrite and extends the file CRC with `crc_extend_zeros`; after the last block it `ftruncate`s to the accumulated offset so trailing holes survive. Honoured on both the pipeline (`pipeline::options.sparse`, io_worker classifies before reading, compress workers pass sparse blocks through unhashed-but-zero-stored) and the serial `add_file_impl` paths.
- **Retry (Phase 8)**: `src/util/retry.h` `retry_io(op, max_attempts=5)` — exponential backoff (1 ms doubling, 100 ms cap), EINTR retried without consuming attempts, `retry_sleep_count()` atomic test hook. Applied to pipeline open/fstat/pread, the serial block pread, and `reader::impl::read_at`.
- **Verify (Phase 8)**: `reader::verify_all()` (public, `include/arch/archive.h`) CRC-checks every data block into a `/dev/null` sink (extract_data `sink` param skips pwrite targets + ftruncate); returns `"name: what()"` per failing entry; hardlink dups verify through the owner's blocks. `parch verify` prints entries to stdout, exit 1 on any failure.
- **Compressor plugin system**: factory registry (`register_compressor` / `make_compressor`) with `REGISTER_COMPRESSOR` macro for static init. Each compressor works on single 4 MiB blocks; codec **and level are pinned per TOC section** (Phase 1). Readers resolve codecs via `make_compressor_by_id(id, level)` (registry probe + id→name cache).
- **Reader (Phase 4 + Phase 10, `src/archive/reader.cpp`)**: constructor runs the full metadata validation chain but reads **no data blocks** (TOC CRCs cover metadata only). `extract_all` runs four **pooled** waves (all on `reader::options.depth` workers, jobs in archive-layout order with by_path last-wins dedup) — dirs bucketed by path depth (parents before children, parallel within a bucket) → files/symlinks/FIFOs/devices (thread-local decompressor cache, first-error capture) → hardlinks via `link()` (falls back to extracting the owner's blocks) → directory metadata reverse-order (children must be in place first). Two concurrency knobs: `depth` = files in flight (latency), `threads` = CRC+decompress permits via `decode_gate` (CPU); the archive `pread` and output `pwrite` run **outside** the gate (§7.1 amendment). Each file is opened **once** (`extract_one` keeps the fd through `fchmod`/`fchown`/`futimens` + fd-based `util::write_xattrs(fd,…)`/`write_acl(fd,…)` = `fsetxattr`/`acl_set_fd`; the Phase 4 metadata re-open is gone). Blocks verified by CRC32C (compressed) then file checksum (uncompressed chain); extraction names go through the zip-slip guard.
- **Reader profiling (Phase 10)**: `reader::options.profile` enables RAII `span` timers over 7 stages (open/pread/crc/decode/pwrite/meta/permit-wait, atomic `stage_timer`s); report → stderr after `extract_all`/`verify_all` + public `reader::profile_report()` (cumulative across ops). Tuning hints fire on stage-share thresholds (RT >30% → raise `--depth`; permit wait >15% → raise `-j`/lower `-l`; CRC+decode >50% → CPU-bound; pread >50% / pwrite >40% → bandwidth walls). `parch extract/verify --depth N --profile`; `arch_bench` honors `ARCH_BENCH_PROFILE=1`. Phase 10 measured 13.9→210 MiB/s extract under the 500 µs shim (docs/benchmarks.md §3.1).
- **Writer profiling (create/append)**: `writer::options.profile` + public `writer::profile_report()`; report → stderr from `finalize()`. Shared timer mechanism lives in `include/util/profile.h` (`arch::util::profile::{now_ns, stage_timer, span, append_stage_table}`) — the writer owns one 12-slot `stage_timer` array (open / meta / pread / crc / hash / compress / permit-wait / walk / writer-wait / pwrite / dedup / toc); `io::pipeline` gets a `profile_hooks*` in its options (`options.prof`, writer-owned, must outlive the pipeline) so the pipeline's io/compress workers report into the same slots, and the **serial fallback reports into the same slots too** (comparable profiles). fsync time is inside `toc+footer` (single span over `finalize_section`/`write_footer` — never double-counted). `writer wait` = time the single consumer thread blocks in `next_block`, interpreted against **wall** (not busy share) in hints — high writer-wait + low depth ⇒ raise `--depth`; high writer-wait with few big files ⇒ Option B (staged pipeline). Hints also cover compress >45% (raise `-j`/lower `-l`), hash >15% (weigh `--dedup` savings), permit wait >15%, walk >20% (walk RPCs contend with opens at the MDS — streamed walk means it is no longer a barrier), pwrite+toc >30% (`--no-sync`). Profiling never changes archive bytes (`[profile]` tests assert byte-identity on/off).
- **`writer::append()` is a static factory** `static writer append(path, opts = {})` (the old member was impossible — the normal ctor `O_TRUNC`s the file): validates the existing archive, keeps the valid section prefix, `ftruncate`s away the old TOC table + footer, inherits refs/totals and the DEDUP footer flag; the returned writer then streams one more section via the normal add*/finalize path.

### Frozen design decisions (v1 — do not re-litigate without updating PLAN.md)
- **Self-contained TOC sections** — all offsets section-relative; dedup never crosses sections (cross-section dedup removed from roadmap).
- **XXH3-128 dedup identity** — `content_id` is 16 bytes (block_entry 48 B); 64-bit was rejected as silently corrupting on collision.
- **TOC CRC covers metadata only** (header + entries + strings) so `list()` never reads data blocks.
- **Durable append**: data → fsync → TOC → fsync → TOC table + footer → fsync; references past EOF = clean truncation point.
- **Relative paths only** in the string table (rooted at the `add_tree()` argument, or at the optional `store_prefix` tar-style root name); extraction rejects absolute / `..`-escaping names.
- **Symlink targets and hardlinks are first-class** — `file_entry` gains `symlink_target_id` and `link_target_id` (Phase 1 item 11).
- **io_uring is optional**; the default I/O path is portable POSIX threads + `pread`.

### Namespace Conventions
- `arch::format` — on-disk structs and magic constants
- `arch::compress` — compressor base class + factory + zstd impl
- `arch::io` — I/O pipeline; with `ARCH_WITH_URING`: pool + prefetcher backends
- `arch` — writer and reader high-level classes
- `util` — xattr, ACL helpers

## Gotchas & Non-Obvious Patterns

### Phase 8 gotchas (sparse / retry / verify)
- **tmpfs + zero pages**: `pwrite` of an all-zero page is silently converted to a hole on tmpfs — test "data" must be **nonzero bytes** (`pseudo()` ORs in `1`) or hole-preservation assertions misfire. Hole granularity is the 4 KiB page: a "data" tail must be ≥ one full page to stay dense.
- **SEEK_DATA semantics**: `block_is_hole` must ask "is there *no data* in [off, off+len)" via `lseek(fd, off, SEEK_DATA)` (returns `off` itself when off is on data → correctly not-a-hole). `SEEK_HOLE` from a data offset returns the *next* hole start and would wrongly classify data-starting blocks.
- **Short pread vs block size**: a block starting on data reads the whole 4 MiB span (zeros past the last page included) and is stored dense — expected, not a bug. Put test data pages at *block starts* (or last) to keep hole blocks whole.
- `crc32c::Crc32c(nullptr, 0)` is ambiguous — use `crc32c::Extend(0, nullptr, 0)` for the CRC of zero bytes.
- `retry_io` returns partial results (`0 <= r < want`) to the caller to re-loop; do not treat partial as failure. `retry_io(op)` on permanent failure leaves the final errno in `errno` after return.
- `writer::append()` on a v0.3 archive **inherits** `used_sparse` (footer version ≥ 0x0003) so the rewritten footer keeps advertising v0.3 even when the appended section is dense.

### Phase 9 gotchas (benchmarks / latency shim)
- **FORTIFY defeats `open` interposition**: GCC Release builds rewrite two-arg `open(p, O_RDONLY)` into `__open_2` (and `openat` → `__openat_2`), which bypasses a plain `open` shim entirely — `netfs_delay.c` must export `__open_2/__open64_2/__openat_2/__openat64_2` too, or the parent process sees zero injected latency (check with `nm -U <obj> | grep open`).
- **Relative-path opens**: tar-style `openat(dirfd, "name", …)` never matches `NETFS_PREFIX` directly; resolve dirfd via `/proc/self/fd/N` (cwd for `AT_FDCWD`) and re-check the prefix.
- `getrusage(RUSAGE_CHILDREN).ru_maxrss` is an **all-time high-water across every past child** — cross-contaminates sequential tar runs. `bench.cpp::run_child` uses `wait4(pid, …, &rusage)` for exact per-child numbers.
- Self-process `ru_maxrss` is monotonic for the whole process: an `--op=all` run reports the same peak for create/extract/verify — read the delta between runs, not the per-op column.
- **Never edit a bash script while it is running** (byte-offset shift corrupts mid-stream parsing — cost a full matrix rerun).
- All timings are **warm page cache** (no root for `drop_caches`); documented as methodology in `docs/benchmarks.md`.
- Bench data lives in `$HOME/benchdata` (override `DATA=`); reuse with `--no-gen` (generation of `small` alone is ~20k file creations).

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
- **Linking note**: `REGISTER_COMPRESSOR` uses static initializers — consumers linking `libarchiver_lib.a` statically must use `-Wl,--whole-archive` (or reference a `zstd_impl` symbol) or `make_compressor("zstd")` fails at runtime. The `parch` CLI does this via literal flag items around the library in `target_link_libraries` (order-preserving).
- **Store mode**: `options.compressor == "none"` is honoured on every write path (pipeline via `use_compression=false`, serial `add_file_impl`/`add_file(data)` via raw pass-through with `compressor_id = COMPRESSOR_NONE`); don't reintroduce an unconditional `make_compressor` call. `[none]` test guards this.

### `reader.cpp` — Phase 4 + Phase 10 notes
- **Parsers live in `src/archive/format_parse.h`** (field-by-field BE, mirroring `docs/format.md`; also used by `writer::append`). Tests keep *independent* re-parsers — a symmetric writer/reader bug must not hide.
- **Metadata CRC recompute needs no re-serialization**: read the raw section bytes, copy the 128-byte header with bytes [92,96) zeroed, CRC over that + the contiguous tail from `file_table_offset` to section end. Compare against BOTH `toc_header.metadata_crc` and the `toc_section_ref.metadata_crc`.
- **Section bounds checks come before any tail read** (`parse_sections`) — a malicious/rotten header must never cause an out-of-range `pread` length.
- **`by_path` is last-occurrence-wins** (append = versioning); `list()` still reports every entry from every kept section.
- **`block_entry.offset` is section-relative from the `toc_header`** and the data region is `[data_offset, data_offset+data_size)`; extract validates blocks against it.
- **Hardlink owner resolution** goes through `by_path` (the owner may live in an earlier section); if the owner path doesn't exist on disk at wave-3 time (overwritten by a later regular file, or section dropped), wave 3 extracts the owner's blocks under the duplicate's name instead of linking.
- **Decompressors are per-thread** (`thread_local codec_cache` — zstd CCtx is not thread-safe; Phase 10 pools recreate threads per wave, hence thread_local not pool-indexed). The reader shares one `arc_fd` across threads via `pread` only.
- **`extract_all` wave order is load-bearing**: parents before children (workers don't mkdir-race), files before hardlinks (link target must exist), children before directory metadata (a ro-mode dir would block writing into it, and times must be set after contents).
- xattr restore: `util::write_xattrs(dest, strings.substr(xattr_id))` — the blob is self-terminating (4-byte zero length), so slicing to the end of the string table is safe. ACL restore is the same pattern (`util::write_acl(dest, strings.substr(acl_id))`, files + wave-4 dirs).
- **Phase 10 gotchas**:
  - **Single-fd handoff**: `extract_one` opens the output once (`fd_guard` RAII) and passes the fd to `extract_data(loc, first, count, out_fd, dest, codecs, sink)`; metadata/xattr/ACL then go through the same fd (`fchmod`/`fchown`/`futimens`, `fsetxattr`, `acl_set_fd`). `verify_all` calls it with `out_fd = -1, sink = true` — no fd at all (the old `/dev/null` open is gone). The fd overloads `util::write_xattrs(int, …)` / `util::write_acl(int, …)` share the blob walk with the path versions; `write_acl(fd, …)` **ignores a default-ACL slot** (files can't carry one; dirs stay on the path route in wave 4).
  - **Gate discipline**: `decode_gate` (counting_semaphore, permits = `options.threads`) wraps only CRC+decompress. Never move the archive `pread` or output `pwrite` inside it — that re-couples the two knobs and reintroduces the Lustre/GPFS problem (§7.1). `gate` is built in the reader ctor before `open_and_parse` (0 threads → hardware_concurrency).
  - **Wave-1 mkdir ordering**: buckets keyed by path-component count, ascending, `run_jobs` within a bucket. Do not replace with plain sorted serial or unordered pooled — parents must exist and workers must not race on the same path.
  - **`run_jobs` (anon namespace)** is the shared first-error pool for all waves + verify. `verify_all` keeps per-job error slots so reported order matches the serial walk (`[phase10]` parity test) — collecting under a mutex in completion order would break test expectations.
  - **Profile spans**: RAII `span(this, STG_x)`; cost when off = one bool test. Counters are **cumulative** across ops on a reader (`profile_report()` after extract+verify shows both). Hints fire at fixed thresholds — the "raise --depth" hint is expected on local NVMe for many-small-file workloads where contention (not RTs) inflates `create+open` (docs/benchmarks.md §3.1).
  - **Wave-3 single-`extract()` path**: extracting a hardlink dup via `reader::extract()` (not `extract_all`) resolves the owner through `by_path` and extracts the owner's blocks under the dup's name (no `link()` — matches old fallback semantics).

### CLI (`src/main.cpp`)
- Diagnostics/progress → stderr (spdlog stderr sink), `list` rows → stdout (pipeable). `verify` failing-entry lines also go to **stdout** (one per line, pipeable). Plain `list` is `tar tf`-style — one stored path per line, nothing else; `list -v` is `tar tvf`-style — perms, `user/group` (via `getpwuid_r`/`getgrgid_r` with per-id cache, numeric fallback — never stored in the archive, resolved at display time), size, date, path, ` -> link-target` — plus the entry/byte summary on stderr. Options parse before *or* after the command word. `-C DIR` chdirs **before anything** (archive path included, like `tar -C DIR -cf …`). Directory args go through `add_tree` **tar-style** — the CLI passes the arg's final path component as `store_prefix`, so `create a.arc dir` stores `dir`, `dir/...` (trailing slash irrelevant; a final `.` component, e.g. `-C dir .`, opts back into contents-only storage); file args → `add_file` (basename storage — same-name files collide, like tar without dirs). Options include `--no-sparse` (clears `writer::options.sparse`; archive stays v0.2), `--no-xattr` / `--no-acl` (skip the per-file metadata probes — see `writer::options.xattrs`/`.acls`). **No proactive stat pass**: `create`/`append` print "parch creating <arc>" immediately and report entry counts + input size afterwards from `writer::contents()` (the old pre-walk for the summary line cost a second full metadata pass). Exit codes: 0 / 1 (runtime or verify failure) / 2 (usage). `test/test_cli.cpp` shells the binary out via `ARCH_CLI_BINARY="$<TARGET_FILE:parch>"` — keep that define in test/CMakeLists.

### `xattr.cpp` — ACL notes (Phase 7)
- **ACL blob layout** (docs/format.md §ACL Storage): `access_text NUL [default_text NUL]` + the writer's trailing `NUL` terminator — slot position decides the type (libacl text carries no `default:` marker). `read_acl` stores nothing for mode-equivalent (trivial) access ACLs; when only a default ACL exists it stores a mode-derived base-3 placeholder so the default lands in slot 1. Blobs are self-terminating: callers slice `strings.substr(acl_id)` like xattr blobs.
- **libacl gotchas**: `acl_set_file` fails **EINVAL** on extended entries without a mask — call `acl_calc_mask`, but only when named entries exist (adding a mask to base-3 text converts a trivial ACL into an extended one on disk). Capture with `acl_to_any_text(..., TEXT_NUMERIC_IDS)` so restores don't need the source's user names.
- **`read_xattrs` skips `system.posix_acl_access`/`_default`** — ACLs have their own field; don't let them leak into the xattr blob again.

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
- The old design routed files < 16 MiB (`BLOCK_SIZE * 4`) through an io_uring within-file prefetcher. **Replaced** by the Phase 3 file-level pipeline (concurrency across files); `use_prefetch` and the threshold are gone with it — the standalone prefetcher survives only as an `ARCH_WITH_URING` component with its own tests, not wired into `add_file`.

## Test Conventions
- Catch2 v3. `TEST_CASE("name", "[tag1][tag2]")`.
- `[slow]` tag for anything >5s, >64 MiB alloc, or requiring network FS.
- Use `Catch::Matchers::Equals` for vector comparison.
- Tests use `mkstemp` for temp files (not Catch2's file helpers).

## Phase Checklist (current status)
See PLAN.md for full status. In short: **Phases 0–5, 7–10 ✅** (builds green; `format.h` on v0.3-minimal (v0.2 layouts + `COMPRESSOR_SPARSE`); writer streams + serializes spec-conformant archives with pipelined `add_tree`; reader parses/validates/extracts in a parallel wave pipeline with corruption + truncation-tolerance semantics; `writer::append()` static factory works and is covered by two-section roundtrip tests — 97/97 default, 103/103 `ARCH_WITH_URING=ON`; reader is ASan/UBSan-clean and TSan-clean, incl. Phase 10); CLI ✅ (`parch create/append/list/extract/verify`, subprocess-tested, ASan-clean via `[cli]` tests); ACL roundtrip ✅ (libacl, access + default ACLs archived and restored, `[acl]` tests); sparse files + retry + verify ✅ (Phase 8, `test/test_sparse.cpp` 14 cases, ASan/UBSan/TSan-clean); benchmarks + memory profile ✅ (Phase 9, `bench/` + `docs/benchmarks.md` — 28× pipeline create win under simulated netfs latency, extract beats tar everywhere, dedup RAM 36 GiB/PiB). **All v1 phases complete.** Optional **Phase 10** ✅ (2026-11: Option A of `docs/extract-pipeline.md` implemented — single-fd handoff + fd-based xattr/ACL + `reader::options{depth,threads}` two-knob pools + parallel `verify_all` + `--profile` timing report; extract under simulated netfs latency 13.9 → **210 MiB/s** at depth 32, docs/benchmarks.md §3.1; Option B still gated on real-NFS bandwidth data).
