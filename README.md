# parch — parallel archiver

A C++20 archiver library and command-line tool built for **network
filesystems** (NFS, Lustre, GPFS). On a netfs every `open`/`stat`/`read` is a
network round trip, so tar-style sequential archiving is latency-bound long
before it is bandwidth-bound. `parch` (the name says it: *parallel*) hides
those round trips with a worker pipeline that reads **many files at once** —
measured ~28× faster archive creation than tar under 500 µs simulated latency
(see `docs/benchmarks.md`).

It uses its own seek-based on-disk format (`docs/format.md`, v0.3), not
libarchive, with pluggable per-block compression and content dedup.

## Highlights

- **Cross-file I/O pipeline** — N files opened and `pread` concurrently by
  worker threads; a bounded-permit design keeps RAM usage predictable
  regardless of tree size (dedup index ≈ 36 GiB per PiB of data).
- **Two independent knobs** — `--depth` (files in flight = latency hiding)
  and `-j` (compression/decompression workers = CPU); the same split on
  extract.
- **Pluggable block compression** — zstd (default) or store, 4 MiB blocks,
  codec + level pinned per TOC section; extensible via a compressor
  registry (`REGISTER_COMPRESSOR`).
- **Integrity** — CRC32C per block plus per-file checksums over the
  uncompressed chain; `parch verify` checks every block without extracting.
- **Within-section dedup** (`--dedup`) — XXH3-128 content identity.
- **Rich metadata** — mode, numeric uid/gid, mtimes, symlinks, hardlinks,
  FIFOs/devices, xattrs and POSIX ACLs (access + default), sparse files
  (holes preserved on extract).
- **Durable, append-friendly format** — self-contained TOC sections;
  `parch append` adds a new section atomically (crash = clean truncation
  point), and later sections version earlier ones (last-write-wins).
- **Robust against transient errors** — exponential-backoff retry on all hot
  syscalls (`src/util/retry.h`).
- **Profiling built in** — `--profile` prints a per-stage timing breakdown
  with concrete tuning hints for both create and extract.
- Optional **io_uring backend** (`-DARCH_WITH_URING=ON`) for the I/O pool and
  within-file prefetcher; the default path is portable POSIX threads + pread.
- Zip-slip-safe extraction, tolerant of truncated/corrupt archives
  (references past EOF are handled as clean truncation).

## Building

Requirements: CMake ≥ 3.22 and a C++20 compiler (tested with GCC 15). The
only required system dependency is **libacl** (`libacl-devel` /
`libacl-dev`); everything else (spdlog, zstd, crc32c, xxHash, Catch2) is
fetched automatically via FetchContent.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure    # 98 tests
```

The CLI lands at `build/parch`. Optional flags:

| Flag | Effect |
|---|---|
| `-DARCH_WITH_URING=ON` | build the io_uring backend + its tests (needs liburing) |

Benchmarks (`bench/run_bench.sh` runs the full matrix against tar, with an
LD_PRELOAD latency shim simulating a netfs):

```bash
cmake --build build --target arch_bench netfs_delay
DATA=$HOME/benchdata bench/run_bench.sh       # results in docs/benchmarks.md
```

## Using the CLI

```
usage: parch create  [opts] <archive> <tree|file...>   build a new archive
       parch append  [opts] <archive> <tree|file...>   add a section to one
       parch list    [opts] <archive>                  show entries
       parch extract [opts] <archive> [dest]           unpack (dest = ".")
       parch verify  [opts] <archive>                  check all block + file CRCs
```

### Examples

```bash
parch create project.arc project            # tar-style: stores "project/..."
parch create -c zstd -l 19 -j 8 --depth 64 backup.arc /data/trees
parch create -c none backup.arc dir         # store mode (no compression)
parch create -C project . proj.arc          # contents of project/, without the
                                            # leading "project/" component
parch append updates.arc newfile.txt        # add a section; old data untouched
parch list backup.arc                       # one path per line (pipeable, tar tf)
parch list -v backup.arc                    # perms user/group size date path -> target
parch extract backup.arc /restore/here      # parallel waves, parents before children
parch verify backup.arc                     # read + CRC every block, write nothing
```

Directory arguments are stored tar-style under their own name; plain file
arguments are stored by basename. Names go through an absolute-path / `..`
escape guard on extract.

### Options

| Option | Meaning |
|---|---|
| `-c, --compressor NAME` | `zstd` (default) or `none` (store) |
| `-l, --level N` | compression level (default 3) |
| `-j, --threads N` | compression / extraction worker threads (default 4) |
| `--depth N` | files in flight — I/O pipeline for create, extract/verify pool (default 32) |
| `--profile` | per-stage timing report + tuning hints on stderr |
| `--dedup` | within-section content dedup (XXH3-128) |
| `--no-sync` | skip fsync barriers (faster, less durable) |
| `--no-sparse` | store holes as zero-filled blocks (archive stays format v0.2) |
| `-C, --directory DIR` | chdir before anything — archive and inputs resolve there (like `tar -C`) |
| `-v, --verbose` | debug logging; `list` shows the detailed rows |
| `-q, --quiet` | errors only |
| `--version` / `-h, --help` | as expected |

Options may appear before or after the command word. Exit codes: **0** ok,
**1** runtime/IO/verify failure, **2** usage error. Diagnostics go to
stderr; `list` rows and `verify` failures go to stdout so they stay
pipeable.

Tuning in one line: on a high-latency filesystem raise `--depth` (it costs
only bounded memory); if `--profile` says compression dominates, raise `-j`
or lower `-l`.

## Repository layout

```
include/arch/   public API headers (format, compressor, archive, io pipeline)
include/util/   xattr/ACL helpers + profiling primitives
src/archive/    writer / reader / shared format parsers
src/compress/   compressor factory + zstd implementation
src/io/         worker pipeline, sparse helpers (+ io_uring backend, opt-in)
src/util/       xattr implementation, retry helpers
src/main.cpp    the parch CLI
test/           Catch2 suite (one file per subsystem)
bench/          benchmark harness + netfs latency shim
docs/           format spec (format.md), API reference, test plan, benchmarks
PLAN.md         phased design + status of everything above
```
