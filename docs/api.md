# Archiver API Reference

Format version **0.2** (design freeze 2026-11).

The archiver library exposes three layers:

1. **Public API** — `arch::writer` and `arch::reader` (`include/arch/archive.h`)
2. **Format types** — on-disk layout constants and in-memory structs (`include/arch/format.h`)
3. **Internal API** — low-level I/O, compression, utilities (not public; may change)

The `parch` command-line tool (`src/main.cpp`, section 8) is the reference
consumer of the public API.

---

## 1. Archive Writer

**Header:** `#include <arch/archive.h>`

### `writer::options`

```cpp
struct options {
    std::string compressor      = "zstd"; // compressor name (registered via factory)
    int         compression_level = 3;    // codec level; pinned into the TOC section header
    int         threads           = 4;    // block-compression pool size
    std::size_t pipeline_depth    = 32;   // files in flight in the stat/open/read pipeline
    bool        dedup             = false;// enable within-section content dedup
    bool        durable           = true; // fsync ordering on finalize/append
    bool        sparse            = true; // archive holes as v0.3 sparse blocks
    bool        xattrs            = true; // probe + capture extended attributes
    bool        acls              = true; // probe + capture POSIX ACLs
    bool        profile           = false;// per-stage create timing report (see profile_report())
};
```

- `compressor` — name looked up in the compressor factory. The special value
  `"none"` selects **store mode**: blocks are written raw with codec ID 0 and
  no factory lookup happens (works on every write path — pipeline and serial).
  Any other name throws `std::runtime_error` at construction if unknown.
- `compression_level` — passed to the compressor. Recorded in every TOC section
  header (format 0.2) together with the codec ID, upholding the determinism
  contract: *same input + same codec + same level ⇒ byte-identical output*.
- `threads` — sizes the **block-compression pool**: workers compress ready
  blocks from *all* in-flight files concurrently. This is the single
  parallelism knob; codecs run single-threaded internally (zstd is pinned to
  `ZSTD_c_nbWorkers=1`). Replaces the 0.1 semantics of "threads passed to the
  compressor".
- `pipeline_depth` — how many files the I/O pipeline keeps in flight
  (stat → open → read across files). This — not within-file readahead — is the
  network-FS latency mechanism. Replaces the old `prefetch_window`.
  Implemented (`arch::io::pipeline`); `threads = 0` selects the serial
  fallback path.
- `dedup` — when true, each uncompressed block is **XXH3-128** hashed before
  compression; duplicates share physical storage **within the section**.
  Requires a codec honouring the determinism contract. Default `false`.
- `durable` — when true, `finalize()`/`append()` enforce the write order
  data → fsync → TOC → fsync → TOC table + footer → fsync. Setting false
  trades crash-durability for speed (single fsync at the end).
- `sparse` — when true (default), blocks of a file whose `fstat` proves it
  sparse (`st_blocks * 512 < st_size`) are checked with `SEEK_DATA` and whole
  holes are stored as **v0.3 sparse blocks** (`COMPRESSOR_SPARSE`, zero
  stored bytes; see `docs/format.md`). Extraction recreates the holes
  (interior holes stay unallocated; a trailing hole is restored via
  `ftruncate`). Hole blocks carry the same `content_id` as dense all-zero
  blocks of equal size, so they dedup against each other. Setting false
  stores hole bytes like any other data and the archive stays format v0.2.
  Honoured on both the pipeline and serial write paths.
- `xattrs` / `acls` — when true (default) every file entry captures extended
  attributes and POSIX ACLs. On network filesystems the probes
  (`llistxattr`/`lgetxattr`, two `acl_get_file` RPCs per file) are pure
  metadata-server traffic — often the dominant create cost when the answer is
  "nothing" (scratch Lustre/GPFS without SELinux labels or ACLs). Setting
  either false skips the probe **and** stores nothing for that class
  (`xattr_id`/`acl_id` stay 0); extraction then leaves the target's
  inherited/default ACLs as-is. Symlinks are never probed either way.
- `profile` — when true, every stage of the creation pipeline is timed
  (source open, xattr/ACL reads, block pread, CRC, XXH3 hashing, compress,
  block-permit wait, tree walk, writer-thread wait, archive pwrite, dedup
  lookup, TOC + fsync) and a report with tuning hints is printed to stderr
  by `finalize()`. The pipelined and serial paths report into the same
  stage slots, so profiles of both are directly comparable. Off cost at a
  call site is one boolean test; the archive bytes are identical either way.

### `writer::writer(path, options)`

Opens (or creates) the archive file at `path` in write-truncate mode. Writes
the 16-byte archive header magic (`ARCH\x01\x00`).

**Throws** `std::system_error` if `open()` fails.

### `writer::~writer()`

Closes the file descriptor if still open. Does **not** auto-finalize — the
archive is invalid if `finalize()` was not called.

### `void add_file(path)`

Archives a filesystem object at `path`. Automatically reads:
- `fstat()`/`statx()` → mode, uid, gid, mtime, atime, nlink
- `listxattr()` / `getxattr()` → extended attributes
- ACL text (`getfacl` form, pending implementation)

Storage rules:
- **Regular files** are read in 4 MiB blocks, each compressed independently and
  **streamed to disk as produced** (only TOC metadata stays in RAM — writer
  memory is bounded regardless of archive size; Phase 2).
- **Symlinks** store their target in `symlink_target_id` (string table). The
  0.1 code read the target and discarded it — a bug fixed by the format.
- **Hardlinks**: repeated `add_file()` calls on distinct paths sharing an inode
  are only detected by `add_tree()` (which keeps an inode→path map). A
  standalone `add_file()` archives the data again.
- **Paths** are stored as given; callers should pass names relative to a
  logical root (see `add_tree`). Names with `..` elements are rejected at
  extraction.

**Throws** `std::system_error` on any filesystem error.

### `void add_file(path, data)`

Archives in-memory data. Path is stored verbatim as the entry name. Data is
split into 4 MiB blocks, compressed, and CRC32C checksummed.

### `void add_tree(root, store_prefix = {})`

Recursively archives the directory tree at `root`:
- Entry names are stored **relative to `root`** (no leading `/`, no `..`).
- Directories become metadata entries; devices/fifos/sockets as in `add_file`.
- Hardlink duplicates share one data set via `link_target_id` (inode→path map).
- The walk STREAMS into the I/O pipeline: each file is submitted the moment
  discovery reaches it (up to `pipeline_depth` files stat/ed/opened/read
  concurrently), so archive bytes are written while the tree is still being
  walked and discovery round-trips overlap with data flow. Regular files are
  not stat'ed during discovery at all — classification comes from readdir
  `d_type` and hardlink identity from `(dir dev, d_ino)` (both ride the
  directory reads; on Lustre that means no per-file MDT getattr), with the
  pipeline's post-open `fstat` as the size/mode/times authority; `DT_UNKNOWN`
  or `d_ino == 0` filesystems (older NFS) fall back to lstat classification.
  Only directories, symlinks, specials and dup-candidates pay a getattr.
  This is why recursion is a first-class operation rather than a caller-side
  loop (a serial loop re-creates the per-file network round-trip cost the
  archiver exists to hide).
- With a non-empty `store_prefix` (tar-style storage of the top level): the
  root itself becomes a directory entry named `store_prefix` (stored first,
  like tar lists `dir/`) and every descendant is stored under
  `store_prefix/...`. The prefix must be a plain relative path without
  `.`/`..` elements (`std::runtime_error` otherwise); a multi-component
  prefix is fine — extraction creates missing intermediate directories.

**Throws** `std::system_error` on any filesystem error.

### `void finalize()`

Flushes all in-memory state to disk, per the format spec:
1. Append `file_entry[]`, `block_entry[]`, string table, dedup index
   (field-by-field big-endian — structs are never `memcpy`'d)
2. Patch the `toc_header` at the section start (sizes + metadata CRC32C)
3. Append the TOC table
4. Write the 256-byte footer at EOF−256 (self-check + signature CRC)

With `durable = true` the step order is data → fsync → TOC → fsync → TOC
table + footer → fsync, then the fd is closed.

With `profile = true`, `finalize()` also prints the per-stage create report
to stderr (see below).

### `std::string profile_report() const`

With `options.profile`: a human-readable per-stage timing breakdown of
everything added so far plus tuning hints — the writer-side counterpart of
`reader::profile_report()`. Stages: `source open` (open+fstat round-trips),
`xattr+read` (metadata reads), `source read` (block preads + `SEEK_DATA`
hole scans), `crc32c`, `xxh3 dedup` (hashing), `compress`, `permit wait`
(I/O workers blocked on in-RAM block permits — reads outrunning the
downstream), `tree walk` (streaming discovery round-trips:
readdir/lstat/readlink, counted per item), `writer wait`
(the single consumer thread blocked on producers), `archive write`,
`dedup lookup`, and `toc+footer` (serialization + fsync barriers). The
header line reports file/byte counters, dedup hits + bytes saved, wall
time and worker utilization. Hints map dominant stages to knobs:
walk/open/metadata share → `--depth` (or note that the FS's metadata rate
is the floor), compress share → `-j` / lower `-l`, hashing share vs. saved bytes
→ drop `--dedup`, permit wait → `-j` or destination write wall, writer
wait → `--depth` (many files) or the staged pipeline for a few huge files
(`docs/extract-pipeline.md` §4), write+TOC share → `--no-sync`. Cumulative
across `add_file`/`add_tree`/`finalize`; empty string when profiling is
off; never affects the archive bytes.

### `summary contents() const`

Content added to this writer so far — `{files, dirs, others, bytes}` —
derived from the buffered TOC entries with **zero filesystem
round-trips** (`bytes` sums uncompressed sizes of regular-file entries;
hardlink duplicates count as files, symlinks/devices/FIFOs as `others`).
Call **before** `finalize()` (the entry buffer is released with the
section). The CLI uses this to report totals *after* creation instead of
walking the tree up front for the counts — on a metadata-rate-bound
network FS that proactive pass costs tens of thousands of MDS RPCs for a
log line.

### `static writer append(path, opts = {})`

Opens an **existing** finalized archive for appending and returns a writer
positioned to extend it. Validates the full footer chain (magic, signature
CRC, self-check CRC, version) plus the TOC-table CRC, then re-verifies every
existing section's metadata CRC. A section that fails validation (or whose
ref extends past the TOC table) marks a **clean truncation point**: it and
all later sections are dropped. The old TOC table + footer are then discarded
(`ftruncate`) and the kept section refs + totals are inherited, so subsequent
`add_file`/`add_tree` calls stream a new **self-contained** TOC section at the
old footer position and `finalize()` rewrites table + footer in the durability
order above. The footer's `DEDUP` flag is sticky (OR-ed with the inherited
value). Dedup does not cross sections.

```cpp
auto w = arch::writer::append("backup.arc");   // default options
w.add_tree("./delta");
w.finalize();                                   // archive now has 2 sections
```

**Throws** `std::system_error` if the file cannot be opened, `std::runtime_error`
if it is not a valid archive.

**Versioning semantics:** a repeated path in a later section is **not** removed
from the listing (`list()` shows both), but extraction applies last-occurrence-
wins — the newest version of the name is what lands on disk.

---

## 2. Archive Reader

**Header:** `#include <arch/archive.h>`

### `reader::reader(path, opts = {})`

`reader::options`:

```cpp
struct options {
    int  threads = 0;    // CRC+decompress permits (CPU knob); 0 → hardware concurrency
    int  depth   = 32;   // extract_all/verify_all workers: files in flight (latency knob)
    bool profile = false; // per-stage timing report, see profile_report()
};
```

Two independent concurrency knobs (Phase 10, `docs/extract-pipeline.md`):
`depth` sizes the per-file worker pools that hide output-side round-trips
(create/open, metadata) on network filesystems — raise it when the profile
shows RT stalls; `threads` caps concurrent CRC+decompress work — the CPU
budget. Archive `pread`s run outside the `threads` gate so source-I/O
concurrency scales with `depth`. A single `extract()` call always blocks the
calling thread. With `profile`, a per-stage timing breakdown plus tuning
hints is printed to stderr at the end of `extract_all`/`verify_all` and is
also available via `profile_report()` (empty string when off).

Opens the archive read-only. Validates the footer (self-check + signature
CRC), reads the TOC table, and parses **section metadata only** (headers,
entry arrays, string tables) into an in-memory listing — data blocks are never
read here, because the TOC CRCs cover metadata only (format 0.2 decision).

Validation performed here (no data blocks are read): footer magic, footer
signature CRC, footer self-check CRC, format version (≤ 0.2), TOC-table CRC,
per-section `TOC1` magic + header bounds + metadata CRC (recomputed from raw
bytes with the header's crc field zeroed). A TOC-section ref that extends past
the TOC table is treated as a clean **truncation point**: that section and all
later ones are dropped, not an error (crash-recovery rule).

**Throws** `std::system_error` if the file cannot be opened.
**Throws** `std::runtime_error` if the footer is absent/invalid.

### `std::vector<listing_entry> list() const`

Returns metadata for every entry across all TOC sections. Fields:

| Field | Type | Notes |
|---|---|---|
| `path` | `std::filesystem::path` | Relative member name |
| `type` | `format::file_type` | `regular`, `symlink`, `directory`, `blkdev`, `chrdev`, `fifo`, `sock` |
| `size` | `std::uint64_t` | Uncompressed size |
| `mode` | `std::uint16_t` | Permission + type bits |
| `uid` | `std::uint32_t` | Owner UID |
| `gid` | `std::uint32_t` | Owner GID |
| `mtime` | `std::uint64_t` | Modification time (seconds) |
| `link_target` | `std::string` | Symlink target or hardlink canonical path; empty if neither |

### `void extract(arc_path, dest)`

Extracts a single entry identified by its archive path to `dest`. Verifies
block-level CRC32C before decompression and entry-level CRC32C after.
Restores mode, times, xattrs, and ACLs; re-creates symlink targets and
hardlinks (`link()` against the already-extracted canonical path).

Names that are absolute or contain `..` path elements are **rejected**
(zip-slip guard) — the whole entry fails with `std::runtime_error`.

Blocks the calling thread until the entry is fully extracted.

### `void extract_all(dest)`

Extraction pipeline — the mirror of the Phase 3 write side, with the same
network-FS rationale (per-file latency hidden by running **files in
parallel**, each file's blocks streamed in order). Four waves:

1. **Directories** created on the `depth`-wide pool, bucketed by path depth
   (shallowest bucket first) so parents always exist before children and no
   two workers `mkdir` the same path.
2. **Regular files, symlinks, FIFOs, devices** extracted on the same pool in
   archive **layout order** (sequential source reads); each worker owns its
   codec instances (thread-local — decompressors are not thread-safe). The
   output file is opened **once** and its fd is reused for data, metadata
   (`fchmod`/`fchown`/`futimens`) and xattr/ACL restore (`fsetxattr`,
   `acl_set_fd`) — no re-open per file (Phase 10). CRC+decompress runs under
   the `options.threads` permit gate; the archive `pread` and the output
   `pwrite` sit outside it. The first failing job is captured and rethrown
   after the pool drains; remaining jobs stop early.
3. **Hardlink duplicates** linked (`link()`) to their canonical owner, also
   pooled; if the owner is missing (dropped section), the owner's blocks are
   extracted under the duplicate's name instead.
4. **Directory metadata** (mode/owner/times/xattrs/ACLs) applied in reverse
   order on the pool, after all children are in place.

Paths are resolved through the last-occurrence-wins map, so a
multi-section (appended) archive extracts its newest versions. Empty archive
regions never touched. Archive structure preserved rooted at `dest`.

### `std::vector<std::string> verify_all() const`

Full data-integrity check without writing any output: reads and CRC-verifies
every block of every regular-file and hardlink entry (decompression included)
into a discard sink, then checks each file checksum. Runs on the same
two-knob pool as `extract_all` (`depth` files in flight, `threads` decode
permits); reported errors keep the same order as the serial walk. Returns one
error string per failing entry (`"name: reason"`, block index included where
relevant); empty means the data is intact. Metadata-level validity
(footer/TOC-table/section CRCs) is already enforced by the constructor, so
`verify_all()` is the complementary *data-block* pass. Hardlink duplicates
are verified through their owner's blocks (mirrors `extract_one`).

```cpp
arch::reader r("backup.arch");
for (const auto& err : r.verify_all()) std::cerr << err << "\n";
```

### `get_xattrs(arc_path)`

Returns xattr key/value pairs for an entry.

```cpp
std::vector<std::pair<std::string, std::vector<std::uint8_t>>> get_xattrs(path);
```

### `get_acl(arc_path)`

Returns the POSIX.1e ACL text in `getfacl` form (numeric IDs). When a default
ACL is stored its entries are prefixed `default:`; empty string when the entry
has no (non-trivial) ACL.

---

## 3. Compressor Interface (public for extension)

**Header:** `#include <arch/compressor.h>`

### `compressor` (abstract base)

```cpp
class compressor {
public:
    virtual ~compressor() = default;
    virtual std::uint8_t id() const = 0;                     // codec magic byte
    virtual std::string_view name() const = 0;               // human-readable name
    virtual compressed_block compress(std::span<const std::uint8_t> input) = 0;
    virtual std::vector<std::uint8_t> decompress(
        std::span<const std::uint8_t> input,
        std::uint32_t expected_uncompressed) = 0;
    virtual std::size_t mem_usage_hint() const { return 0; } // per-thread estimate
};
```

Each call to `compress()` receives exactly one block (≤ 4 MiB). Returns a
`compressed_block` containing the compressed data, CRC32C, and codec ID.
Codecs are constructed with a level and are always invoked single-threaded —
parallelism comes from the writer's block-compression pool, not from the codec.

**Determinism contract:** for any input `X`, `compress(X)` must return
byte-identical output on every call and every conforming implementation build,
given the same codec ID and level. Required for dedup verification (a duplicate
block's `crc32c` is the owner's). `zstd_impl` satisfies this (no adaptive
strategy across blocks); custom codecs must opt in or forbid `dedup`.

`decompress()` receives the raw compressed bytes and the expected uncompressed
size. Returns an empty `vector` on failure (corrupted data, size mismatch).

### `compressed_block`

```cpp
struct compressed_block {
    std::vector<std::uint8_t> data;       // codec output
    std::uint32_t crc32c{};               // CRC32C of `data`
    std::uint8_t compressor_id{};         // matches compressor::id()
};
```

### Factory

```cpp
std::unique_ptr<compressor> make_compressor(std::string_view name, int level);
void register_compressor(std::string_view name,
                         std::function<std::unique_ptr<compressor>(int level)> factory);
```

`make_compressor("zstd", 3)` → zstd at level 3. Throws if unknown.
(The 0.1 signature took no level; the level is now explicit and recorded in the
TOC header.) Factories are invoked while the registry mutex is held — they must
not re-enter `register_compressor`/`make_compressor`.

`"none"` is a **reserved keyword**, not a registered codec: the writer
short-circuits it into raw pass-through (ID 0) before ever calling the
factory, and `make_compressor("none")` throws. Readers resolve stored blocks
by ID via `make_compressor_by_id(id, level)` (registry probe + ID→name cache);
ID 0 needs no codec.

```cpp
std::unique_ptr<compressor> make_compressor_by_id(std::uint8_t id, int level);
```

### Registering a new compressor

```cpp
class lz4_impl : public compressor {
public:
    explicit lz4_impl(int level);
    std::uint8_t id() const override { return 0x10; }
    std::string_view name() const override { return "lz4"; }
    // ...
};
REGISTER_COMPRESSOR("lz4", lz4_impl);
```

The static initializer calls `register_compressor` once. After that,
`make_compressor("lz4", level)` works.

---

## 4. I/O layer (internal, documented for reference)

**Not public API — subject to change.**

### Primary path: `arch::io::pipeline` (built — Phase 3)

```cpp
// Actual interface (include/arch/pipeline.h). Consumption contract:
//   seq = submit(path, size_estimate)          // blocks while files_in_flight full
//   while (next_block(seq, i, blk)) {          // ordered per-file block delivery
//       ...write/inspect blk...; release_block(seq);   // frees a memory permit
//   }
//   fin = wait_file_done(seq);                 // terminal record: stat + xattrs + error
class pipeline {
public:
    struct options {
        int files_in_flight = 32;   // concurrent open/pread threads (writer: pipeline_depth)
        int compress_threads = 4;   // compression pool (writer: options.threads)
        int block_permits = 96;     // max live blocks in RAM (memory bound)
        int per_file_cap = 2;       // liveness bound per in-flight file
        bool sparse = true;         // emit COMPRESSOR_SPARSE hole blocks
        bool xattrs = true;         // io worker: read_xattrs per file
        bool acls = true;           // io worker: read_acl per file
        profile_hooks* prof = nullptr; // optional writer-owned stage timers
    };
    pipeline(options, std::string compressor, int level,
             bool use_compression, bool dedup);
    ...
};
```

Worker threads run `statx` → `openat` → `pread` over **many files at once**;
this is the portable POSIX implementation of the network-FS fast path and the
*default and only* I/O mechanism in a standard build.

### Optional backend: `io::io_uring_pool` (`ARCH_WITH_URING=ON`)

```cpp
using io_callback = std::function<void(int fd, void*, int res)>;

class io_uring_pool {
public:
    io_uring_pool(unsigned sq_depth = 1024, unsigned cq_depth = 1024,
                  unsigned workers = 4);
    ~io_uring_pool();

    int submit_read(int fd, void* buf, unsigned len, uint64_t off, void* ud, io_callback cb);
    int submit_write(int fd, const void* buf, unsigned len, uint64_t off, void* ud,
                     io_callback cb);
    int submit_statx(int fd, const char* path, void* statxbuf, unsigned flags,
                     unsigned mask, void* ud, io_callback cb);
    int submit_fsync(int fd, void* ud, io_callback cb);
    int submit_openat(int dirfd, const char* path, int flags, void* ud, io_callback cb);
    int submit_close(int fd, void* ud, io_callback cb);

    void drain_and_stop();
};
```

- Callback parameters: `fd` (unused, `-1`), `ud` (pointer to submission tag),
  `res` (bytes transferred, or negative errno on failure).
- Not thread-safe for concurrent submissions from multiple threads; the caller
  must serialize `submit_*` calls or provide its own locking.
- `drain_and_stop()` blocks until all pending CQEs are processed, then
  terminates worker threads.
- Intended end state: one implementation of the pipeline interface, chosen at
  build time, for local-NVMe workloads. Until then it is legacy/experimental.

### Optional backend: `io::prefetcher` (`ARCH_WITH_URING=ON`)

Within-one-file readahead on `io_uring_pool` (window 1 MiB, 64 KiB chunks,
ordered delivery via `pop_ready_chunk()`). Kept compiling, but superseded
conceptually by the file-level pipeline — the archiver no longer treats
in-file readahead as the latency mechanism.

---

## 5. Utility functions (internal)

### `util::read_xattrs(path, out)`

Reads POSIX extended attributes from `path`, appends them as a binary blob to
`out`, returns the blob offset. Returns `0` if no xattrs.

### `util::write_xattrs(path, blob)`

Restores xattrs from a previously-saved binary blob.

### `util::read_acl(path, out)` / `util::write_acl(path, blob)`

POSIX.1e ACL capture/restore on libacl (implemented — Phase 7). `read_acl`
stores non-trivial access ACLs and directory default ACLs as the concatenated
NUL-terminated text blob in docs/format.md §ACL Storage; mode-equivalent ACLs
are skipped (return 0). `write_acl` applies slot 0 as the access ACL and slot 1
as the default ACL; filesystems without ACL support (EOPNOTSUPP) only log.

---

## 6. Format constants

**Header:** `#include <arch/format.h>`

| Constant | Value | Units |
|---|---|---|
| `format::ARCH_BLOCK_SIZE` | 4 MiB | bytes |
| `format::COMPRESSOR_NONE` | `0x00` | byte |
| `format::COMPRESSOR_ZSTD` | `0x01` | byte |

`ARCH_BLOCK_SIZE` replaces the old name `BLOCK_SIZE`, which collided with the
`BLOCK_SIZE` macro from `sys/mount.h`.

On-disk layout is **canonical field tables in `docs/format.md`**, always
big-endian, never struct-backed `memcpy`. In-memory sizes of the helper structs
are asserted, but the authoritative record sizes are:

| Struct | On-disk size |
|---|---|
| archive header | 16 bytes |
| footer | 256 bytes |
| `toc_section_ref` | 24 bytes |
| `toc_header` | 128 bytes |
| `file_entry` | 128 bytes (fixed — includes `symlink_target_id`, `link_target_id`) |
| `block_entry` | 48 bytes (16-byte XXH3-128 `content_id`) |
| `dedup_entry` | 32 bytes (16-byte `content_hash`) |

> `format.h` and the size tests are ported to the v0.2 layouts (PLAN.md
> Phase 1 done): `digest128` = 16-byte `content_id`/`content_hash`, 128 B
> `file_entry`, 128 B `toc_header` with codec + level + `metadata_crc`,
> canonical `*_SIZE` constants with `static_assert` guards. Field-by-field
> **serialization** lands with Phase 2.

The helpers `htobe*_safe`/`betoh*_safe` in `format.h` dispatch on
`__BYTE_ORDER__` — byte-swap on little-endian hosts, identity on big-endian.

---

## 7. Example usage

### Create an archive

```cpp
#include <arch/archive.h>

arch::writer::options opts{};
opts.dedup            = true;
opts.threads          = 4;
opts.pipeline_depth   = 64;   // many-small-files over NFS: raise this
opts.compression_level = 5;

arch::writer w("backup.arc", opts);
w.add_tree("/srv/data");                          // names relative to "data"
w.add_tree("/srv/data", "data");                  // tar-style: "data/..." entries
w.add_file("notes.txt", std::string_view("Hello, World!")); // in-memory entry
w.finalize();
```

### List an archive

```cpp
arch::reader r("backup.arc");
for (auto& e : r.list()) {          // metadata only — fast even on NFS
    std::cout << e.path << "  " << e.size << "  " << std::oct << e.mode << "\n";
}
```

### Extract an archive

```cpp
arch::reader r("backup.arc");
r.extract_all("/tmp/restore");      // symlinks + hardlinks re-created,
                                    // zip-slip names rejected
```

### Add a custom compressor

```cpp
// In your .cpp:
class noop_impl : public arch::compress::compressor {
public:
    explicit noop_impl(int /*level*/) {}
    std::uint8_t id() const override { return 0x11; }
    std::string_view name() const override { return "noop"; }

    arch::compress::compressed_block compress(std::span<const std::uint8_t> input) override {
        arch::compress::compressed_block b;
        b.data.assign(input.begin(), input.end());
        b.crc32c = crc32c::Crc32c(input.data(), input.size());
        b.compressor_id = 0x11;
        return b;
    }

    std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> input,
                                          std::uint32_t expected) override {
        if (input.size() != expected) return {};
        return {input.begin(), input.end()};
    }
};
REGISTER_COMPRESSOR("noop", noop_impl);
```

(`"noop"` is just an example name — don't register under `"none"`, that is the
writer's reserved store-mode keyword. The real google/crc32c entry points are
`crc32c::Crc32c(data, size)` / `crc32c::Extend(...)` — there is no
`crc32c::Value`.)

---

## 8. Command-line tool (`parch`)

**Source:** `src/main.cpp` — built as the `parch` target. All diagnostics and
progress go to **stderr** (spdlog, colored); `list` rows go to **stdout** so
they are pipeable. Exit codes: `0` success, `1` runtime/ I/O error, `2` usage
error. Options may appear before *or* after the command word.

```
usage: parch create  [opts] <archive> <tree|file...>
       parch append  [opts] <archive> <tree|file...>
       parch list    [opts] <archive>
       parch extract [opts] <archive> [dest]      (dest default: .)
       parch verify  [opts] <archive>             (check all block + file CRCs)

opts:
  -c, --compressor NAME   zstd (default) or none (store)
  -l, --level N           compression level (default 3)
  -j, --threads N         compression / extraction workers (default 4)
      --depth N           files in flight: I/O pipeline (create) / extract+verify
                          workers (default 32)
      --profile           per-stage timing report + tuning hints on stderr
                          (create, append, extract, verify)
      --dedup             within-section content dedup (XXH3-128)
      --no-sync           skip fsync barriers (faster, less durable)
      --no-sparse         store holes as zero-filled blocks (archive stays v0.2)
      --no-xattr          skip extended-attribute capture (fewer metadata RPCs)
      --no-acl            skip POSIX ACL capture (fewer metadata RPCs)
  -C, --directory DIR     chdir to DIR before anything: the archive path and all
                          inputs resolve from there (like tar -C; e.g. store the
                          contents of a dir without its name: -C DIR . )
  -v, --verbose           debug logging
  -q, --quiet             errors only
      --version           print version and exit
  -h, --help              this text
```

- `create` / `append` map the options 1:1 onto `writer::options`
  (`append` uses the `writer::append()` static factory — one new TOC
  section per invocation); `--profile` sets `writer::options.profile` and
  the create report is printed by `finalize()`. **Directory** arguments are
  stored tar-style via `add_tree(name, prefix)`: the directory's own final
  path component becomes the entry prefix, including a top-level directory
  entry (`parch create a.arc dir` stores `dir`, `dir/a.txt`, …; a trailing
  slash makes no difference; `..` resolves to the parent's real name). A
  final `.` component stores **only the contents** (the old behavior):
  `parch create a.arc -C dir .`. Plain **file** arguments go through
  `add_file` and are stored by their basename (same-basename files from
  different directories collide, like `tar` without leading dirs). `-C`
  chdirs before anything else, so the **archive path is resolved from it
  too** (like `tar -C DIR -cf a.tar …`); a bad `-C` dir exits 1.
- `list` reads metadata only (no data blocks) and prints one row per entry
  in `ls -l`-style: type+permissions, size, mtime, path, and ` -> target`
  for symlinks/hardlinks.
- `extract` maps `-j` onto `reader::options.threads` (when given; otherwise
  the reader defaults to hardware concurrency) and `--depth` onto
  `reader::options.depth` (files in flight across the four waves);
  `--profile` sets `reader::options.profile`. Runs the `extract_all`
  pipeline.
- `verify` maps the same reader options (`-j`, `--depth`, `--profile`) and
  runs the constructor's metadata validation plus `reader::verify_all()`;
  one failing entry per line on **stdout**, summary on stderr; exit `0` when
  clean, `1` when any entry fails.
- `--no-sparse` clears `writer::options.sparse` (holes stored densely, archive
  stays format v0.2).

**Linking note:** the `parch` target wraps `archiver_lib` in a literal
`-Wl,--whole-archive ... -Wl,--no-whole-archive` pair inside
`target_link_libraries` so the `REGISTER_COMPRESSOR` static initializers
survive linking against the static library.
