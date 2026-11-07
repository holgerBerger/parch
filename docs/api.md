# Archiver API Reference

Format version **0.2** (design freeze 2026-11).

The archiver library exposes three layers:

1. **Public API** — `arch::writer` and `arch::reader` (`include/arch/archive.h`)
2. **Format types** — on-disk layout constants and in-memory structs (`include/arch/format.h`)
3. **Internal API** — low-level I/O, compression, utilities (not public; may change)

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
};
```

- `compressor` — name looked up in the compressor factory. Throws
  `std::runtime_error` at construction if unknown.
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

### `void add_tree(root)`

Recursively archives the directory tree at `root`:
- Entry names are stored **relative to `root`** (no leading `/`, no `..`).
- Directories become metadata entries; devices/fifos/sockets as in `add_file`.
- Hardlink duplicates share one data set via `link_target_id` (inode→path map).
- The walk feeds the I/O pipeline: `pipeline_depth` files are stat/ed/opened/
  read concurrently — this is why recursion is a first-class operation rather
  than a caller-side loop (a serial loop re-creates the per-file network
  round-trip cost the archiver exists to hide).

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
    int threads = 0;   // extract_all worker pool size; 0 → hardware concurrency
};
```

`threads` bounds the **extract_all** per-file worker pool (see below); a
single `extract()` call always blocks the calling thread.

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

1. **Directories** created serially (sorted) so workers never race on `mkdir`.
2. **Regular files, symlinks, FIFOs, devices** extracted on the worker pool
   (`options.threads`, default `hardware_concurrency`); each worker owns its
   codec instances (one decompressor per thread — decompressors are not
   thread-safe). The first failing job is captured and rethrown after the
   pool drains; remaining jobs stop early.
3. **Hardlink duplicates** linked (`link()`) to their canonical owner; if the
   owner is missing (dropped section), the owner's blocks are extracted under
   the duplicate's name instead.
4. **Directory metadata** (mode/owner/times/xattrs) applied in reverse order,
   after all children are in place.

Paths are resolved through the last-occurrence-wins map, so a
multi-section (appended) archive extracts its newest versions. Empty archive
regions never touched. Archive structure preserved rooted at `dest`.

### `get_xattrs(arc_path)`

Returns xattr key/value pairs for an entry.

```cpp
std::vector<std::pair<std::string, std::vector<std::uint8_t>>> get_xattrs(path);
```

### `get_acl(arc_path)`

Returns the POSIX.1e ACL text (same format as `getfacl` output).

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

### `util::read_acl(path, out)` / `util::write_acl(path, text)`

POSIX.1e ACL getfacl/setfacl wrapper. Pending implementation.

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
w.add_tree("/srv/data");                          // relative names under "data/"
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
    std::string_view name() const override { return "none"; }

    arch::compress::compressed_block compress(std::span<const std::uint8_t> input) override {
        arch::compress::compressed_block b;
        b.data.assign(input.begin(), input.end());
        b.crc32c = crc32c::Value(reinterpret_cast<const char*>(input.data()), input.size());
        b.compressor_id = 0x11;
        return b;
    }

    std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> input,
                                          std::uint32_t expected) override {
        if (input.size() != expected) return {};
        return {input.begin(), input.end()};
    }
};
REGISTER_COMPRESSOR("none", noop_impl);
```

(`crc32c::Value` is the real google/crc32c entry point — the 0.1 doc used a
non-existent `crc32c::Crc32c`.)
