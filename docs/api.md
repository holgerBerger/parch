# Archiver API Reference

The archiver library exposes three layers:

1. **Public API** — `arch::writer` and `arch::reader` (`include/arch/archive.h`)
2. **Format types** — on-disk structs and constants (`include/arch/format.h`)
3. **Internal API** — low-level I/O, compression, utilities (not public; may change)

---

## 1. Archive Writer

**Header:** `#include <arch/archive.h>`

### `writer::options`

```cpp
struct options {
    std::string compressor      = "zstd";  // compressor name (registered via factory)
    int         threads         = 4;       // parallel compression threads
    bool        dedup           = false;   // enable content-aware deduplication
    std::size_t prefetch_window = 16;      // small-file prefetch batch count
};
```

- `compressor` — name string looked up in the compressor factory. Default is `"zstd"`.
  Throws `std::runtime_error` at build time if the name is unknown.
- `threads` — passed to the compressor (e.g. `ZSTD_c_nbThreads` for zstd). `0` means
  auto-detect.
- `dedup` — when true, each uncompressed block is XXH3-64 hashed before compression.
  Blocks with identical content share physical storage. Adds ~1 × XXH3 pass + hash-table
  overhead. Default `false`.
- `prefetch_window` — for small files (< 16 MiB heuristic), the number of blocks to
  keep in flight via io_uring read-ahead.

### `writer::writer(path, options)`

Opens (or creates) the archive file at `path` in write-truncate mode. Writes the 16-byte
archive header magic (`ARCH\x01\x00`).

**Throws** `std::system_error` if `open()` fails.

### `~writer::~writer()`

Closes the file descriptor if still open. Does **not** auto-finalize — the archive is
invalid if `finalize()` was not called.

### `void add_file(path)`

Archives a filesystem object at `path`. Automatically reads:
- `fstat()` → mode, uid, gid, mtime, atime, nlink
- `listxattr()` / `getxattr()` → extended attributes
- ACL text (via `getfacl`, pending implementation)

For regular files, data is read in 4 MiB blocks, each compressed independently. Large
file reads use `pread()` directly. Small files (< 16 MiB) trigger the prefetcher path.

**Throws** `std::system_error` on any filesystem error.

### `void add_file(path, data)`

Archives in-memory data. Path is stored verbatim as the entry name. Data is split into
4 MiB blocks, compressed, and CRC32C checksummed.

### `void finalize()`

Flushes all in-memory state to disk:
1. Compressed data region
2. Block entry array
3. File entry array
4. String table (null-terminated paths)
5. TOC section header (with CRC32C)
6. Footer (256 bytes, with signature CRC)

Calls `fsync()` before closing the file descriptor.

### `void append(path)`

Opens an **existing** finalized archive, reads and validates the footer, appends a new
TOC section at EOF containing newly added files, and rewrites the footer. Not yet
implemented.

---

## 2. Archive Reader

**Header:** `#include <arch/archive.h>`

### `reader::reader(path)`

Opens the archive in read-only mode. Validates the footer magic, reads the TOC table,
and parses all TOC sections into an in-memory entry list.

**Throws** `std::system_error` if the file cannot be opened.
**Throws** `std::runtime_error` if footer magic/Checksum is invalid (future).

### `std::vector<listing_entry> list() const`

Returns metadata for every entry across all TOC sections. Fields:

| Field | Type | Notes |
|---|---|---|
| `path` | `std::filesystem::path` | Archive member name |
| `type` | `format::file_type` | `regular`, `symlink`, `directory`, `blkdev`, `chrdev` |
| `size` | `std::uint64_t` | Uncompressed size |
| `mode` | `std::uint16_t` | POSIX mode bits |
| `uid` | `std::uint32_t` | Owner UID |
| `gid` | `std::uint32_t` | Owner GID |
| `mtime` | `std::uint64_t` | Modification time (seconds) |

### `void extract(arc_path, dest)`

Extracts a single entry identified by its archive path to `dest`. Verifies block-level
CRC32C before decompression, entry-level CRC32C after. Restores mode, times, xattrs,
and ACLs.

Blocks the calling thread until the entry is fully extracted.

### `void extract_all(dest)`

Iterates `list()` in order. Creates directory entries first, then extracts regular files.
Preserves the archive directory structure rooted at `dest`.

### `get_xattrs(arc_path)`

Returns xattr key/value pairs for an entry.

```cpp
// Returns: { {"user.selinux", { 0x73, 0x79, 0x73, 0x74, 0x65, 0x6d, 0x75, 0x00 }}, ... }
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

`decompress()` receives the raw compressed bytes and the expected uncompressed size.
Returns an empty `vector` on failure (corrupted data, size mismatch).

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
std::unique_ptr<compressor> make_compressor(std::string_view name);
void register_compressor(std::string_view name,
                         std::function<std::unique_ptr<compressor>()> factory);
```

`make_compressor("zstd")` → returns zstd. Throws if unknown.

### Registering a new compressor

```cpp
class lz4_impl : public compressor {
    std::uint8_t id() const override { return 0x10; }
    std::string_view name() const override { return "lz4"; }
    // ...
};
REGISTER_COMPRESSOR("lz4", lz4_impl);
```

The static initializer calls `register_compressor` once. After that,
`make_compressor("lz4")` works.

---

## 4. I/O layer (internal, documented for reference)

**Not public API — subject to change.** Low-level `io_uring` wrapper and prefetcher.

### `io::io_uring_pool`

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
- Not thread-safe for concurrent submissions from multiple threads; the caller must
  serialize `submit_*` calls or provide its own locking.
- `drain_and_stop()` blocks until all pending CQEs are processed, then terminates
  worker threads.

### `io::prefetcher`

```cpp
class prefetcher {
public:
    prefetcher(io_uring_pool& pool,
               size_t window_size = 1 * 1024 * 1024,
               size_t chunk_size  = 64 * 1024);
    void start(int fd, std::uint64_t file_size);
    std::optional<std::pair<std::uint64_t, std::vector<std::uint8_t>>>
    pop_ready_chunk();
    bool is_done() const;
};
```

- `start(fd, size)` — begin async reads from `fd` at offset 0.
- `pop_ready_chunk()` — blocking pull of the next chunk in offset order. Returns
  `{offset, data}`. Returns `std::nullopt` when EOF is reached and all chunks consumed.
- `is_done()` — true when the file has been fully prefetched and all chunks delivered.

---

## 5. Utility functions (internal)

### `util::read_xattrs(path, out)`

Reads POSIX extended attributes from `path`, appends them as a binary blob to `out`,
returns the blob offset. Returns `0` if no xattrs.

### `util::write_xattrs(path, blob)`

Restores xattrs from a previously-saved binary blob.

### `util::read_acl(path, out)` / `util::write_acl(path, text)`

POSIX.1e ACL getfacl/setfacl wrapper. Pending implementation.

---

## 6. Format constants

**Header:** `#include <arch/format.h>`

| Constant | Value | Units |
|---|---|---|
| `format::BLOCK_SIZE` | 4 MiB | bytes |
| `format::COMPRESSOR_NONE` | `0x00` | byte |
| `format::COMPRESSOR_ZSTD` | `0x01` | byte |

On-disk structs are **always big-endian**. The helpers `htobe*/betoh*` in `format.h`
handle conversion. Struct sizes are `static_assert`-verified:

| Struct | Size |
|---|---|
| `block_entry` | 32 bytes |
| `footer` | 256 bytes |
| `file_entry` | 148 bytes (variable depending on compiler padding) |
| `dedup_entry` | 24 bytes |

---

## 7. Example usage

### Create an archive

```cpp
#include <arch/archive.h>

auto opts = arch::writer::options{};
opts.dedup   = true;
opts.threads = 4;

arch::writer w("backup.arc", opts);
w.add_file("/var/log/syslog");
w.add_file("/etc/nginx/nginx.conf");
w.add_file("data.txt", std::string_view("Hello, World!"));
w.finalize();
```

### List an archive

```cpp
arch::reader r("backup.arc");
for (auto& e : r.list()) {
    std::cout << e.path << "  " << e.size << "  " << std::oct << e.mode << "\n";
}
```

### Extract an archive

```cpp
arch::reader r("backup.arc");
r.extract_all("/tmp/restore");
```

### Add a custom compressor

```cpp
// In your .cpp:
class noop_impl : public arch::compress::compressor {
    std::uint8_t id() const override { return 0x10; }
    std::string_view name() const override { return "none"; }

    arch::compress::compressed_block compress(std::span<const std::uint8_t> input) override {
        arch::compress::compressed_block b;
        b.data.assign(input.begin(), input.end());
        b.crc32c = crc32c::Crc32c(input.data(), input.size());
        b.compressor_id = 0x10;
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
