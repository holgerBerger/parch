#pragma once

#include "arch/format.h"
#include "util/profile.h"
#include <sys/stat.h>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace arch::io {

/// One compressed block handed to the writer, in file order.
struct pipeline_block {
    std::vector<std::uint8_t> compressed;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t crc32c = 0;        // CRC32C of the *stored* (compressed) bytes
    std::uint8_t  compressor_id = 0;
    format::digest128 content_id{};  // XXH3-128 of raw block (zero if dedup off)
};

/// Terminal per-file record: status + metadata the I/O stage collected.
struct pipeline_file {
    std::filesystem::path path;
    std::error_code error;           // open/fstat/pread failure (surfaces in submit order)
    struct stat st{};                // fstat taken by the I/O stage (zeroed on error)
    std::uint32_t file_crc32c = 0;   // CRC32C of the whole *uncompressed* stream
    std::vector<std::uint8_t> xattr_blob;
    std::vector<std::uint8_t> acl_blob;
};

/// File-level I/O + compression pipeline (PLAN Phase 3, items 18/19).
///
/// Plain-POSIX backend: `files_in_flight` worker threads run open → fstat →
/// pread on that many *files* concurrently — network-FS latency is hidden by
/// cross-file concurrency, not within-file readahead. Each raw block is
/// hashed (dedup) and pushed to a queue that `compress_threads` workers drain
/// with single-block compression; results land in per-file ordered slots.
///
/// Consumption contract (the writer is the single consumer, in submit order):
///   seq = submit(...)                  // blocks while `files_in_flight` full
///   while (next_block(seq, i, blk))    // waits for block i of file `seq`
///       ... pwrite / dedup ...
///       release_block(seq)             // frees one memory permit
///   fin = wait_file_done(seq)          // every returned block got its
///                                      // release_block; frees the in-flight slot
/// next_block returns false at the first missing/failed block; short files
/// simply produce fewer blocks than submit()'s estimate.
///
/// Memory is bounded by `block_permits` live blocks (raw in queue, compressed
/// in slots, or held by the writer, ≤ 4 MiB each). Each in-flight file may
/// hold at most `per_file_cap` permits, which keeps the head file's I/O
/// thread from starving behind the other in-flight files (no deadlock: the
/// cap guarantees the oldest file can always make progress).
///
/// Determinism: compressed bytes depend only on the pinned codec+level
/// (docs/format.md determinism contract) and the writer assigns output
/// order, so archive bytes never depend on worker scheduling.
class pipeline {
public:
    /// Optional per-stage timers (writer-owned array; see util/profile.h).
    /// Null pointer = that stage not measured. The writer's serial fallback
    /// path reports into the same slots, so profiles are comparable.
    struct profile_hooks {
        util::profile::stage_timer *open = nullptr,        // open+fstat per file
            *meta = nullptr,                               // xattr/ACL reads
            *pread = nullptr,                              // raw block reads
            *crc = nullptr,                                // file checksum CRC
            *hash = nullptr,                               // XXH3-128 dedup hash
            *permit_wait = nullptr,                        // block-permit stall
            *compress = nullptr;                           // codec work
    };

    struct options {
        int files_in_flight = 32;   // concurrent open/pread threads (writer pipeline_depth)
        int compress_threads = 4;   // block-compression pool (writer options.threads)
        int block_permits = 96;     // max live blocks (raw or compressed) in RAM
        int per_file_cap = 2;       // max live blocks owned by one file (liveness bound)
        bool sparse = true;         // emit hole blocks as COMPRESSOR_SPARSE (v0.3)
        profile_hooks* prof = nullptr; // optional stage timers (outlives the pipeline)
    };

    /// @param compressor     codec name for the factory registry
    /// @param level          compression level, pinned for every block
    /// @param use_compression false → blocks pass through uncompressed
    /// @param dedup          compute XXH3-128 content ids
    explicit pipeline(options opts, std::string compressor, int level,
                      bool use_compression, bool dedup);
    ~pipeline();

    pipeline(const pipeline&) = delete;
    pipeline& operator=(const pipeline&) = delete;

    /// Queue one regular file for reading. `file_size` is the caller's stat
    /// estimate only — the I/O stage takes its own fstat. Blocks while
    /// `files_in_flight` files are outstanding (the writer must keep draining
    /// the head while submitting). Returns the sequence number for
    /// next_block/release_block/wait_file_done.
    std::uint64_t submit(std::filesystem::path path, std::uint64_t file_size,
                         bool follow_symlinks = true);

    /// Wait for block `index` of `seq` and move it out. Returns false when
    /// index is past the file's produced blocks or the file failed (the error
    /// is reported by wait_file_done).
    bool next_block(std::uint64_t seq, std::size_t index, pipeline_block& out);

    /// Writer is done with one previously returned block (after pwrite).
    void release_block(std::uint64_t seq);

    /// Wait for the I/O stage to finish, take the terminal record, reclaim
    /// any leftover permits and free the in-flight slot. Must be called
    /// exactly once per successful submit().
    pipeline_file wait_file_done(std::uint64_t seq);

    /// Files currently accepted but not yet fully consumed.
    std::size_t in_flight();

private:
    struct block_slot {
        pipeline_block blk;
        bool ready = false;
    };
    struct file_state {
        std::filesystem::path path;
        std::size_t nblocks = 0;
        bool follow_symlinks = true;
        bool io_done = false;
        std::error_code error;
        struct stat st{};
        std::uint32_t file_crc = 0;
        std::vector<std::uint8_t> xattr_blob, acl_blob;
        std::vector<block_slot> slots;
        // Permit accounting (exactly-once release, see class comment):
        int permits_held = 0;  // blocks owned: queued raw + in-compress + slots + writer
        int queued_raw = 0;    // subset: sitting in m_raw_queue
        int in_compress = 0;   // subset: popped by a compression worker
    };
    struct raw_block {
        std::uint64_t seq = 0;
        std::size_t index = 0;
        std::vector<std::uint8_t> data;
        format::digest128 content_id{};
        // Sparse (hole) block: `data` is empty, `usize` carries the logical
        // block length; the compression stage emits COMPRESSOR_SPARSE.
        bool sparse = false;
        std::uint32_t usize = 0;
    };

    void io_worker();
    void compress_worker();

    options m_opts;
    std::string m_compressor;
    int m_level;
    bool m_use_compression;
    bool m_dedup;

    std::vector<std::thread> m_io_threads, m_comp_threads;
    std::atomic<bool> m_stopping{false};

    std::mutex m_mtx;
    std::condition_variable m_cv;       // everything: slots, queues, permits
    std::deque<std::pair<std::uint64_t, std::shared_ptr<file_state>>> m_io_queue;
    std::deque<raw_block> m_raw_queue;
    std::map<std::uint64_t, std::shared_ptr<file_state>> m_files;
    std::uint64_t m_next_seq = 0;
    int m_permits_used = 0;
};

} // namespace arch::io
