# Extraction Pipeline Design (Phase 10, optional)

Status: **Option A implemented (2026-11, PLAN item 35)** — measured 15×
extract under the 500 µs shim (13.9 → 210 MiB/s at `depth=32`, 295 at 128;
expected ~5×; full data + local trade-off in `docs/benchmarks.md` §3.1).
Option B (§4) remains gated on a real-NFS bandwidth measurement. Original
motivation below: Phase 9
benchmark (`docs/benchmarks.md` §3, conclusion 6): under 500 µs simulated
netfs latency, `parch create` reaches 97.7 MiB/s (pipeline) but
`extract_all` only 13.9 MiB/s — extraction parallelizes *decompression*
but not the per-file *round-trip storm*, so it stays latency-bound.

Which option, on which filesystem: **A always first** (it owns the
metadata-RT win, which dominates small-file restore everywhere). B is
"gated" on NFS-ish targets (§4) but **effectively required by default
on Lustre/GPFS-class scale-out storage** — see §7.

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
  `parch extract --depth N` (same word, same meaning as `create`).
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
   open/metadata RT chains, 4 concurrent zstd streams. (On Lustre/GPFS,
   move the `pread` *outside* the semaphore — §7.1.)
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

### 3.5 Implemented addition — optional stage profiling (`options.profile`)

Not in the original design: every `depth` worker wraps its stage bodies
in an RAII `span` (`STG_OPEN`, `STG_PREAD`, `STG_CRC`, `STG_DECODE`,
`STG_PWRITE`, `STG_META`, `STG_PERMIT_WAIT`) recording cumulative ns+ops
into relaxed atomics (off cost: one bool test; permit-wait timing is the
one always-on-path measurement guarded by the flag). `profile_report()`
(public) returns a per-stage table + worker utilization + **tuning
hints**:

| signal | hint |
|---|---|
| RT stages (`create+open`+`metadata`) > 30 % of busy | latency-bound → raise `--depth` (at ≥128: the FS metadata rate is the floor) |
| `permit wait` > 15 % of busy | `threads` (decode) is the bottleneck → raise `-j` or lower `-l` |
| `crc32c`+`decompress` > 50 % | CPU-bound → more `-j` / trade ratio for speed |
| `archive read` > 50 % | source bandwidth/latency wall — single-huge-file restore is exactly Option B's gap (§7.2) |
| `write out` > 40 % | destination bandwidth wall → cache hygiene, not threads (§7.2) |
| utilization < 25 % with no dominant stage | stalls *between* spans → raise `--depth` |

The report is emitted to stderr at the end of `extract_all`/`verify_all`
when the flag is on (`parch extract/verify --profile`;
`ARCH_BENCH_PROFILE=1` for the bench). Counters are cumulative across
both ops on one reader. Measured payoff (§3.1 addendum): under the shim
the report shows 97.9 % of busy in `create+open` at 98.5 % utilization —
pure overlapped latency, confirming A's model; raising `-j` 4→16 changes
nothing at `depth=32`, which is the two-knob split working as designed.

**Actual vs §3.3 predictions (2026-11, implemented):** 210 MiB/s at
`depth=32` (predicted 70–85 — the single-fd handoff plus out-of-permit
`pread` beat the extrapolation; 20 000 × 1 RT ÷ 32 ≈ 0.31 s matches the
measured 0.37 s almost exactly). Local warm `small` *regressed* at the
default depth (359 vs 665 MiB/s, §3.1 addendum): on low-latency storage
32 concurrent creators just contend — `--depth 4` recovers ~604 MiB/s.
The profile report identifies this correctly (RT stage dominates but
per-op time is inflated), and the hint text stands as designed.

**Writer-side mirror (2026-11 follow-up, implemented):** the same
mechanism (extracted to `include/util/profile.h`) instruments create/
append via `writer::options.profile` — 12 stage slots covering the
pipeline (open/xattr/pread/CRC/hash/compress/permit-wait, hooked through
`io::pipeline::options.prof`), the serial tree walk, the single consumer
thread (`writer wait`, judged against wall — its hint distinguishes
"many files → raise `--depth`" from "few huge files → Option B's staged
pipeline is the fix", §4), archive pwrite, dedup lookup and toc+fsync.
The serial fallback reports into the same slots, and on/off runs are
byte-identical. So the §4 Option-B gate can now be decided from data:
run `parch create --profile` on the real workload and read which hint
fires.

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

## 7. Scale-out parallel filesystems (Lustre, GPFS) — how the balance shifts

Phase 9's simulation models NFS-ish behavior (latency on open + first
read). Lustre/GPFS at storage scale differ in three economically
important ways:

| resource | NFS-ish | Lustre/GPFS | consequence |
|---|---|---|---|
| metadata ops | ~1 RT each | **1–10 ms MDS/token RT each**, and *more* of them: xattrs live on the MDS, `chmod`/`chown`/`utimens` are one RPC apiece, same-directory creates convoy on the directory lock (Lustre) / inode-space token (GPFS) | metadata RPC count is the small-file currency — **Option A's domain; B adds nothing here** |
| data path | single target, ~free | per-OST/per-node bandwidth ceiling; aggregate = stripes × per-OST; BDP at 100 Gb × ms-RTT ≈ **12–100 MB must be in flight** | in-flight bytes are the large-file currency — **A's permit placement caps them; B owns the knob** |
| decode CPU | slack | ~0.5–1 GB/s/core (zstd-3) ⇒ a 12.5 GB/s client needs 16+ cores decoding while I/O stalls *longer* (ms) | I/O concurrency must be **decoupled** from codec concurrency |

### 7.1 Option A — still necessary, but two design amendments

A captures the entire metadata-side win (double-open removal halves the
create-path MDS ops; fd-based xattr saves a lookup; `depth` overlaps the
irreducible `chmod`/`fchown`/`futimens`/`setxattr` RPC storm — their
*count* is unbatchable on Lustre, only their latency is hideable).
But as specced in §3.2 the `pread` sits **inside** the decode permit:
achieved archive-read concurrency = `threads` (4). At a 5 ms effective
OST RT that is a 4×4 MiB/5 ms ≈ **3.2 GB/s hard ceiling**, with 28 of
32 file workers idling. Amendments for this class of FS:

1. **`pread` outside the permit.** The permit covers CRC + decompress
   (CPU) only; every file worker may then have one block in flight →
   `depth` × 4 MiB ≈ 128 MB, filling the BDP. Memory model becomes
   worst-case `depth × 8 MiB` live buffers (one read-ahead + one
   decoding) — for big-file-heavy trees that needs the same permit
   accounting §3.1 waived, or `depth`-aware block read-ahead.
2. **Job-order vs directory-convoy tension.** §3.2 step 6's
   layout-order jobs optimize the (single, striped) archive stream, but
   at the destination, 32 workers creating files in *the same source
   directory* serialize on one MDS lock. Interleave wave-2 jobs across
   directories (round-robin by parent) — layout order only ever helps
   one fd's readahead, and striped Lustre reads parallelize *within* a
   4 MiB op already (1 MiB stripes ⇒ 4 OSTs per pread — the frozen
   4 MiB block size is stripe-friendly by construction).

Neither amendment fixes the one structural hole: **single-huge-file
restore** is one file = one worker = one cursor ≈ 1–2 concurrent block
RPCs, roughly an order of magnitude off aggregate OST bandwidth on a
big client. That case is Option B's, full stop.

### 7.2 Option B — the gate (§4) is effectively *met by default* here

On NFS-ish targets you might never be bandwidth-bound on restore, and
B can stay hypothetical. On Lustre/GPFS the residual *is* bandwidth
once A's metadata win lands:

- Stage R's `readers` knob (in-flight bytes, independent of CPU) and
  stage C's codec pool sizing are exactly the two dials a fat-NIC
  restore needs — A merges them into worker bookkeeping and can only
  approximate.
- Big-file fan-out: stage R/C/W hand one file's blocks to multiple
  writers (`pwrite` is order-free), so a single archive member streams
  at min(NIC, stripes × per-OST, threads × decode speed) instead of
  one worker's sequential cursor.
- The `block_permits` pool becomes the explicit BDP dial (e.g. 64–128
  × 4 MiB on a 100 Gb client) instead of an emergent property, and it
  is the natural home for the page-cache hygiene this class of
  deployment wants: restore reads of 100 GiB+ archives pollute the
  client cache for nothing — `posix_fadvise(DONTNEED)` after each
  consumed block, or an O_DIRECT stage R, neither of which fits the
  monolithic file-worker model.

### 7.3 What neither option buys

- MDS RPC *count* per file (create + setattr ×3 + setxattr + flush) is
  a protocol property — overlapping is all we can do. A
  `--no-times`/`--no-owner`/bulk-restore switch that skips
  restore_metadata subsets would cut RPCs rather than hide them;
  cheap to add, policy call.
- Hardlinks: `link()` is one cheap MDS op on Lustre — pooling wave 3
  (A step 4) is enough.
- The **writer-side pipeline already fits this class of FS** — it is
  latency-bound across files (A/B don't touch it), and its per-block
  `pwrite` at `depth`-32 is multi-OST parallel on a striped archive:
  one of the format's quiet scale-out advantages.

### 7.4 Benchmark follow-up

`netfs_delay` should grow a *scale-out mode* before B's gate can be
tested at all: delay **setattr/setxattr/unlink/link** (the MDS storm A
overlaps — today only `open` is delayed, which underestimates Lustre
metadata asymmetry), and optionally a token-bucket per-fd bandwidth cap
to reproduce the aggregate-vs-single-stream gap. That plus a
`--workload=hugefile` (single 8 GiB file) row would turn §4's "gate on
measurement" into something measurable; a real Lustre/GPFS mount
remains the only honest validation of B.

### 7.5 Measurement protocol: what decides A vs B on a real Lustre mount

Decision rule to instrument for: **B earns its ~800 LOC iff, after
amended-A (§7.1), the residual restore time is dominated by
single-stream-capped bytes** — i.e. blocks that sit in one file's
sequential cursor while the OSTs idle. Everything below serves that one
attribution.

**Prerequisites (skip these and the comparison is invalid):**

- **P0a. Archive stripe layout** (`lfs getstripe`): the archive must be
  striped (`-c ≥` OST count); a `-c 1` archive is one disk's
  bandwidth for *every* design, client-side included. Restripe or
  re-create before measuring.
- **P0b. Per-client RPC ceiling** (`osc_max_rpc_bytes`, ptlrpc pool
  sizes): Lustre caps one client's in-flight RPC bytes independent of
  thread count. If the cap sits at or below amended-A's natural
  `depth × 4 MiB ≈ 128 MB`, stage R can buy nothing — the client is the
  wall, not the design. Measure by driving fio `numjobs`/`iodepth`
  until throughput flattens from one client only.

**Filesystem characterization (design-independent, no arch code):**

| # | quantity | method | feeds |
|---|---|---|---|
| M1 | metadata op latency **and aggregate ops/s**: create/open, chmod, chown, utimens, setxattr, link, unlink — cold (mdcache evicted) vs warm, single-directory vs spread, idle FS vs loaded | `mdtest`, or extend `netfs_delay`-style timing harness; `lfs df -w` (MDT count), DNE matters | the `depth` at which the MDS *saturates* — beyond it A's concurrency knob is free; below it, A's win is capped by MDS rate, and nothing else matters for small files |
| M2 | bandwidth-vs-concurrency curve: sequential read of a striped file, `bs = 1–4 MiB`, `iodepth = 1…32`, `numjobs = 1…32`; same for buffered writes | `fio` (libaio) | the core curve: single-stream rate `r_1`, aggregate `r_agg`, and the in-flight MB needed to reach each — tells us exactly where A's `depth`-concurrent preads land and what B's stage R could add |
| M3 | zstd-3 decode throughput per core on the *restore client* | `zstd -b3 -i5` | if `cores × decode < r_agg`, CPU is the binding wall — fixed by sizing A's decode permits, no B required |
| M4 | page-cache cost of restore: dataset ≫ RAM, buffered vs `fadvise(DONTNEED)` vs O_DIRECT reads | A runs or fio | whether B's hygiene hooks (§7.2 bullet 3) are a real operational win, or noise |

**Workload reality (from the actual archive sets, not benchmarks):**

| # | quantity | method | feeds |
|---|---|---|---|
| W1 | byte-weighted file-size histogram; fraction of bytes in files whose single-stream read ≥ M2's `r_1`-limited band (files < ~`r_1 × 10 s`, say, are cursor-capped relative to `r_agg`) | `find -printf '%s'` | B's addressable byte fraction — if < 10 %, B is dead on arrival regardless of how good stage R is |
| W2 | dir fan-out (files/dir) + hardlink rate + xattr/ACL presence | walk stats | MDS convoy exposure (§7.1 amendment 2 relevance) |

**Amended-A run + residual attribution (the actual decider):**

| # | quantity | method | verdict mapping |
|---|---|---|---|
| R1 | per-worker stage timers: time blocked in archive `pread`, in decode permit, in output `pwrite`, in open/setattr RTs | instrumented `arch_bench --op=extract` at `depth=32` | where restore minutes actually go |
| R2 | achieved OST RPC concurrency + per-OST utilization during restore | `lctl`/per-OSC proc counters, `iostat` on OSSs | the discriminator: **OSTs idle while R1 shows workers serialized behind one file's cursor ⇒ B pays. OSTs at M2's aggregate ⇒ A+done. Decode cores at M3 ⇒ raise permits, not stages.** |
| R3 | single-huge-file extract rate vs M2's `iodepth ≥ 8` line | one 8 GiB+ archive member | B's fan-out headroom in one number |

**Decision arithmetic.** From M2 + W1, the counterfactual B-saving is
`T_B = Σ_f bytes_f × (1/r_f^A − 1/r_f^agg)` over cursor-capped files.
**Implement B iff `T_B` exceeds ~25 % of amended-A's measured total.**
Below that, A + permit sizing + the §7.1 amendments is the whole
program.

**Portability caveat.** Absolute Lustre numbers never transfer between
sites (MDS generation, DNE fan-out, spinner vs SSD OSTs, fabric). The
transferable quantities are the *ratios*: `r_agg / r_1` from M2 (B's
ceiling) and MDS-saturating `depth` from M1 (A's ceiling). A site that
cannot run this protocol can still be triaged by supplying exactly
those two curves.
