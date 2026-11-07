# Extraction Pipeline Design (Phase 10, optional)

Status: **design** — implementation not started. Motivation: Phase 9
benchmark (`docs/benchmarks.md` §3, conclusion 6): under 500 µs simulated
netfs latency, `arch create` reaches 97.7 MiB/s (pipeline) but
`extract_all` only 13.9 MiB/s — extraction parallelizes *decompression*
but not the per-file *round-trip storm*, so it stays latency-bound.

## 1. Problem statement

Extraction of a 20 000-file tree at 500 µs/file RT cost (warm cache,
NVMe-backed, LD_PRELOAD shim):

| | create (pipeline) | extract (today) |
|---|---|---|
| throughput | 97.7 MiB/s | 13.9 MiB/s |
| wall time | 0.80 s | 5.62 s |

The archive **read** side is not the problem: one fd, block-sized
`pread`s, kernel-pipelined. The problem is the **output file** lifecycle:
every extracted file costs several serialized remote procedure calls that
today's thread pool overlaps by only `threads` (default = CPU count,
`-j 4` from the CLI).

### 1.1 Per-file round-trip budget (regular file, today)

Per `extract_one` (src/archive/reader.cpp), in order:

| # | syscall | NFS cost | note |
|---|---|---|---|
| 1 | `mkdir` check (`create_directories`) | 1 lookup RT | usually exists already (wave 1 made it) |
| 2 | `open(dest, O_CREAT\|O_TRUNC)` (in `extract_data`) | 1 CREATE RT | |
| 3 | `close` | flush RT (async) | |
| 4 | **`open(dest, O_WRONLY\|O_NOFOLLOW)`** (the "reopen" in `extract_one`) | **1 more CREATE-time lookup RT** | **only to get an fd for metadata — the file was just written through a different fd** |
| 5 | `fchmod`/`fchown`/`futimens` | 3 SETATTR RTs | fd-based, but serial |
| 6 | `setxattr(path)` / ACL restore | lookup + SETXATTR RTs | **path-based** — another resolution even though an fd is open |
| 7 | `close` | flush RT | |

≈ 3 guaranteed create/lookup RTs + several SETATTR RTs per file, all
serialized inside one pool worker. 20 000 files × ~1.1 ms ÷ 4 threads ≈
5.5 s — matches the measurement. Decompression CPU (~0.4 s total) and
archive bandwidth are essentially idle meanwhile.

Additionally, waves 1/3/4 (mkdir, `link()`, directory metadata) and
`verify_all` run **entirely serially** — 2 RTs per hardlink duplicate,
4+ RTs per directory in wave 4.

### 1.2 Why the writer doesn't have this problem

`arch::io::pipeline` separates the latency-bound resource (32 file-open
threads) from the CPU-bound one (4 compression workers) and bounds RAM
with a permit semaphore. Extraction has the mirror-image structure and
today collapses both roles into one pool sized by one knob.

## 2. Design invariants (unchanged, from the v1 frozen decisions)

- **Wave order stays load-bearing**: dirs → files/symlinks/FIFOs/devices
  → hardlinks (owner must exist) → directory metadata (children in place
  before chmod-ro + utimensat). This design adds concurrency *inside*
  waves, never reorders across them.
- `block_entry` bounds checks before any `pread`; CRC32C before
  decompression; file checksum after the block loop; zip-slip guard on
  every name; `by_path` last-occurrence-wins dedup of jobs.
- First-error capture + cooperative stop; partial output is *not* rolled
  back (same semantics as today — at most `depth` files truncated at any
  moment, vs `threads` today).
- No byte-determinism requirement on output (unlike the archive): blocks
  land via positional `pwrite`, order within a file is free.
- Per-thread decompressor cache (zstd DCtx is not thread-safe).

## 3. Option A — two-knob pool + fd handoff (recommended, ~150 LOC)

Small, low-risk, captures essentially all of the simulated-netfs win.

### 3.1 API

```cpp
struct reader::options {
    int threads = 0;  // concurrent decode permits; 0 → hardware_concurrency
    int depth   = 32; // files in flight (wave 2/3/4 workers); the latency knob
};
```

- Mirrors `writer::options{threads, pipeline_depth}`; CLI gains
  `arch extract --depth N` (same word, same meaning as `create`).
- Memory model: each file-pool worker holds at most one live block pair
  (compressed + decompressed, ≤ 8 MiB); the **decode semaphore** (size
  `threads`) bounds how many workers may be in the read+decompress
  critical section at once. Worst case RAM = `min(depth, active
  big-files) × 8 MiB` — big files are rare; small files cost ~nothing.
  (Option B is where a `block_permits`-style pool becomes necessary.)

### 3.2 Per-file job lifecycle (wave 2)

```
depth workers, jobs = files.size() entries (by_path-deduped):
  1. open dest O_CREAT|O_TRUNC            ← the RT we now overlap 32-wide
  2. per block j:
       acquire decode-permit[threads]
         pread archive + CRC32C + decompress   ← CPU stays -j bound
       release permit
       pwrite(out_fd, off_j)                   ← positional; no reordering logic
  3. file-CRC check, ftruncate for trailing hole (as today)
  4. restore_metadata(out_fd)  — fchmod/fchown/futimens on the SAME fd
  5. write_xattrs(out_fd) / write_acl(out_fd)  — NEW fd-based overloads
  6. close(out_fd)
```

Changes to code, in order of effect:

1. **Kill the double-open.** `extract_data` returns the output fd instead
   of closing it (sink mode: no fd at all, and skip the `pwrite` into
   `/dev/null` — today's verify opens `/dev/null` per entry). Removes RT
   #4, ~2× fewer create-path RTs per file.
2. **fd-based xattr/ACL restore**: `util::write_xattrs(int fd, blob)` via
   `fsetxattr`; `util::write_acl(int fd, blob)` via `acl_set_fd`
   (regular files only carry an access ACL — default ACLs are a
   directory concept and stay on the path-based wave-4 route). Removes
   the path-lookup RT (#6).
3. **Split the pools**: file pool = `depth` (32) threads; a
   `std::counting_semaphore<int>` around the per-block CRC+decompress
   section keeps CPU parallelism at `threads`. Net effect: 32 concurrent
   open/metadata RT chains, 4 concurrent zstd streams.
4. **Pool waves 1, 3, 4** on the same depth pool (they are pure-RT work:
   `mkdir`, `remove`+`link`, `chmod`/`chown`/`utimensat`/xattr on dirs).
   Wave 1 must keep parents-before-children: bucket jobs by path depth
   and run buckets in order, workers within a bucket — or keep wave 1
   serial and just let `create_directories` in wave 2 absorb deep
   trees (it already does; measure first).
5. **Parallelize `verify_all`**: same job list (regular + hardlink
   entries), sink mode, decode-permit bound, error strings collected
   under a mutex in entry order. Local win alone: `big` verify
   2.5 GiB/s single-threaded → CPU-bound multi-gigabyte/s; it also
   becomes latency-tolerant for free.
6. **Job iteration order**: build the wave-2 job vector in *archive
   layout order* (section, entry) rather than `by_path` order, keeping a
   path→loc map for dedup only. Sequential archive `pread`s even when
   workers are a few files apart — costs nothing locally, is what a real
   NFS client wants (§5).

### 3.3 Expected results

Simulated netfs (`small`, 500 µs RT), extrapolated from §1.1:

| metric | today | Option A | how |
|---|---|---|---|
| create-path RTs / file | ~2.5 | 1 | steps 1–2 |
| RT overlap | 4 | 32 | step 3 (`depth`) |
| extract throughput | 13.9 MiB/s | **~70–85 MiB/s (~5×)** | 20 000 × 0.5 ms ÷ 32 ≈ 0.31 s RT + ~0.5 s CPU |
| verify | serial | ~`threads`× | step 5 |

On real NFS with 5–10 ms RTTs the win scales linearly (it is *all* RTs):
vs `tar --zstd` extract (fully serial, ≥ 3 RTs/file) the gap becomes
order 10–30×. Locally (warm NVMe): extract roughly unchanged (+few %,
it is CPU-bound there) and RAM grows by `depth − threads` worker
stacks/contexts (~40 MiB) — bounded and off the hot path.

### 3.4 Non-goals for Option A

- No speculative open-ahead (files are opened when their job starts, so
  the blast radius of a mid-extract failure is unchanged).
- No separate block-staging RAM pool — the decode semaphore bounds it
  well enough at these shapes (see §4 for when it isn't).
- Public behavior, format, and wave semantics unchanged.

## 4. Option B — staged `arch::io::extract_pipeline` (only if B is needed)

The faithful mirror of the write pipeline, for read-bandwidth-bound
deployments (large files over real NFS/SMB where per-file RTs are no
longer the whole story):

```
 stage R ("io"):  threads_R workers pull BLOCK jobs in archive LAYOUT
                  order → pread → CRC → hand compressed buffer onward
 stage C (codec): threads workers decompress → decoded buffer into the
                  owning file's ordered slot (same slot design as
                  pipeline.cpp)
 stage W (file):  depth workers own one output file each: eager open at
                  submit time, consume slots (pwrite, order-free),
                  metadata via held fd, close
 RAM bound:       block_permits semaphore over live buffers (≤ 4 MiB
                  each, raw or decoded), per-file liveness cap —
                  identical accounting rules as src/io/pipeline.cpp,
                  inverted (1 source file → N output files)
```

- New files: `include/arch/extract_pipeline.h`, `src/io/extract_pipeline.cpp`
  (namespace `arch::io`, always built); reader keeps policy (waves,
  metadata, name checks) and drives it, exactly as `writer` drives
  `pipeline` today.
- Stage R layout-ordered reads make the archive stream fully sequential
  for the NFS client (max rsize pipelining) — the one real win over
  Option A, and invisible in the current simulation (the shim delays
  only the *first* read per fd; re-measure against a real mount first).
- Optional block cache keyed by (section, offset): dedup hits re-read
  the same physical bytes; irrelevant locally, could matter for
  dedup-heavy archives on WAN links.
- Cost: ~600–800 LOC + tests at the pipeline's level of care (its
  consumption contract, permit-exactly-once, and liveness-bound
  gotchas all apply in mirror). **Gate: only after Option A lands and
  a real-NFS benchmark shows read-side bandwidth, not RTs, as the
  residual limiter.**

## 5. Rejected alternatives

- **Just raise `-j`** (one knob, bigger pool): overlaps RTs but couples
  RT concurrency to decompressor count — 32 threads ⇒ 32 live block
  pairs (up to 256 MiB unbounded) and oversubscribed CPU on big files;
  also still pays the double-open and path-setxattr. Fixes 2 of 3
  problems while making RAM a function of the latency knob.
- **Keep the double-open** (reopen for metadata is "cheap"): it is a
  full create-path RT on the target filesystem — one third of the
  measured per-file latency cost. No local advantage either (the
  original fd is already open).
- **Per-block parallelism inside one file** (split big files across
  workers): the pipeline already extracts files in parallel; only
  single-huge-file archives would benefit, and archive reads for one
  file are contiguous anyway. Revisit with Option B's slot machinery if
  ever measured.
- **`pwritev`/batched metadata syscalls** (`openat2`, `setxattr`
  batching): `io_uring`-style submission batching helps on *local*
  high-IOPS setups, not on NFS where each call is an RPC; the frozen
  v1 decision keeps the primary path plain POSIX.

## 6. Testing & rollout plan

- `test/test_reader.cpp` extensions:
  - byte-identity: `depth ∈ {1, 4, 32}` × `threads ∈ {1, 4}` all produce
    the same tree (RT/CPU split must not change semantics);
  - hardlink dup + fallback-extract paths under the fd-handoff change;
  - mid-extract failure (nonexistent parent under a read-only dir,
    corrupt block at file #k): first-error + stop still holds, no fd
    leak (ASan), `depth` partial files max;
  - xattr/ACL roundtrips through the new **fd-based** util overloads
    (both overloads must produce byte-identical results to path-based;
    `[acl]` tag, libacl on tmpfs caveat from AGENTS.md applies);
  - `verify_all` parallel: error list order/content identical to serial
    on the corruption fixtures; TSan + ASan/UBSan clean (the sanitizer
    driver pattern in AGENTS.md).
- Bench: extend `run_bench.sh` §3 with an extract-under-latency row at
  `depth ∈ {4, 32}` — the acceptance test is extract approaching create
  (~70+ MiB/s at 500 µs, vs 13.9 today); record in
  `docs/benchmarks.md` as a Phase 10 addendum.
- Docs: `docs/api.md` reader options + CLI usage (`extract --depth`),
  AGENTS.md Phase 10 gotchas (fd-handoff invariants, decode-semaphore
  accounting).
- Suggested sequencing: 3.2 steps 1–2 (pure win, no API change) →
  step 3 (options.depth) → steps 4–6 → measure → decide on Option B.
